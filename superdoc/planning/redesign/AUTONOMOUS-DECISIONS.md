# Decisions taken without the user

The user went to sleep on **2026-08-23** having said: *"I will go to sleep now, so you will have
to make decisions. This is fine, since you are working on another branch. Write down the
decisions, that you made, so i can confirm that they are fine later!"*

Everything below was decided in their absence and **needs their confirmation**. Newest block on
top. Each entry records what was chosen, what the alternative was, and why — so disagreeing is
cheap.

---

## 2026-08-23

### D16 · Eight calls taken while building the command palette and the keyboard, P4

P4 adds the `Ctrl+K` command palette over every Entry **and every Param**, the shared
arrow-key adjuster behind it, and the rest of the shell's keyboard. Build clean, **68/68
tests** (a new `overlay_palette` suite, 19 cases / 68 assertions). Full write-up:
`round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D16.1 · The two defects P4 was briefed to fix were ALREADY FIXED, and this is recorded
rather than quietly skipped.** The brief named the `display_scale 2.0` console abort as the
most serious open defect. It is not open: P3c's `5582437` ("never rebuild the font atlas
from the console thread") fixed it, exactly as D15.10(b) records, by adding
`fonts::RequestRebuild()` / `PumpRequestedRebuild()` and routing the registered setter
through them. **Verified by triggering it**, not by reading the diff: with the overlay open
and `overlay_e2` on, `overlay_e2_set overlay.display_scale 2.0` leaves the compositor alive
and rescales the shell — the slab goes 1560 → 1728 px and the rail collapses to icons,
which is the ladder's own step 1. The companion "the shell does not re-scale" item that
D15 left open is therefore **also** closed, and by the same commit: the setter reaches
`QueueGeneralSave()` → `PushLiveTheme()` → `palette::g_LiveTheme.flDisplayScale`, which is
the one value `Shell::Draw()` pushes into `ui::SetScale()` every frame. **They were the
same fix**, which is what the brief asked to be told either way. Screenshots at both scales
are in the P4 report.

**D16.2 · A NEW defect at 2.0× is reported, not fixed, because fixing it is a design
decision.** At `display_scale 2.0` the ladder correctly reaches step 2 (icon rail,
Inspector as a **drawer**), and the drawer then paints over the sheet's whole control
column — every segmented control, switch and slider on screen is hidden behind it. That is
what SPEC §8.3's own table asks for ("inspector overlays", sheet keeps its full 804 base),
so it is not a coding slip; it is the design not saying what an always-visible drawer does
to the lane it floats over. **Rejected:** guessing — either shrinking the sheet under a
drawer (which makes the drawer a column by another name and contradicts §8.3's arithmetic)
or auto-hiding the drawer to its spine (which changes what `Ctrl+I`'s three states mean).
Both are real design changes and belong to whoever owns the ladder, not to a drive-by in
the palette's commit.

**D16.3 · The query line is hand-rolled on `io.InputQueueCharacters`, and `InputText` is
not used.** Direction B's `FEASIBILITY.md` §2 reached this first and E2 confirms it against
its own key table. Three reasons, in order of weight: (a) `InputTextEx()` calls
`SetKeyOwner()` on Left/Right while active with no callback to decline them, and SPEC §8.2
gives those two keys to *adjust the highlighted entry's value in place* — the palette's
headline behaviour, which would be unimplementable; (b) Esc on an active `InputText`
**reverts the buffer**, where SPEC's Esc ladder wants the palette dismissed; (c) it buys no
IME, because this ImGui context has no platform backend at all, so
`io.SetPlatformImeDataFn` is null either way. **The cost, stated:** no selection, no caret
placement, no paste, no dead-key composition. All four are absent everywhere else in this
overlay already, and `wlserver` fills `InputQueueCharacters` from the compositor's own xkb
state, so what arrives is layout-correct UTF-8 without `InputText` doing anything for it.

**D16.4 · Browsing IS search with an empty query — enforced by the scorer, not by
convention.** `Score()` returns `kScoreExact` for an empty query, so every item matches at
equal rank and the **stable** sort leaves registration order intact. There is deliberately
no "list everything" branch to drift away from the search branch. This is the one place the
design's "one code path, not two" is a mechanical property rather than a promise, and it is
pinned by two tests (`an empty query lists every entry AND every param`, `an empty query
keeps registration order`).

**D16.5 · The palette needed NO new declaration at any call site — which is the result the
brief asked to have checked.** Every one of the 102 live results comes from what P3a–P3c
already registered. Two getters were added to `Registry.h` (`Entry::KeywordText()`,
`Parameter::KeywordText()`) and that is the whole registry-side change: the `.Keywords()`
**setter** has existed since P1 with no reader at all, so until now a keyword list was a
declaration nothing consumed. No area file was touched. **The one design smell found is the
inverse of the one being looked for:** `.Keywords()` had shipped unread for three phases,
which is how a declaration silently stops being true.

**D16.6 · One adjuster, `AdjustValue()`, shared by the palette and the Sheet — not two.**
The palette's `←→` and a focused row's `←→` are the same function over an `Adjustable` view
of either an `Entry` or a `Parameter`. D15.1's argument about geometry applies unchanged to
behaviour: a slider that steps by one amount in the Sheet and another in the palette is
#25/#68 in a form neither host reveals alone. Two rules inside it are choices worth naming.
**A switch takes its value from the DIRECTION** (Right = on, Left = off) rather than
toggling — holding Right down a list of switches must settle, not oscillate. **A choice
stops at both ends** rather than wrapping, so "press Right until it is what I want" is
reliable.

**D16.7 · The ≤3-character discoverability gate is ADOPTED, as a test and a console
command, not as a build gate.** Direction B failed the *build* when any setting could not be
reached in ≤3 characters. Same property, cheaper and safer here: `WorstCharsToReach()` is a
pure function, asserted in `overlay_palette` against a fixture, and reported over the **live**
registry by `overlay_e2_palette reach`. **Live registry answer today: 2.** **Rejected:**
B's registration abort — a law violation already aborts the compositor (D11.6), and
extending that to *search ranking* means a tuning change to the scorer can take the
compositor down at boot. Discoverability is a quality metric, not a structural law, and the
guillotine should stay pointed at the four laws that are.

**D16.8 · `overlay_e2_palette` exists because the palette is keyboard-only and synthetic
input is permanently forbidden.** D4 bans injected input, so a keyboard-only surface would
be the one feature in the product verifiable *only* by a human at a keyboard — and "renders
but does nothing" is the exact defect class this branch keeps finding. The command drives
the same three variables the keys drive (there is no parallel path), and its `list` verb
prints the ranked results, so the **ranking** is checkable from a script even though the
drawing is not. It is the same argument that produced `overlay_e2_select` and
`overlay_e2_set`, applied to the third piece of shell state.

**Still open, and NOT fixed here.** *(a)* D16.2's 2.0× drawer occlusion, above. *(b)* Real
key events were never sent: `ydotool` is banned and no other injection route exists that
respects that ban, so every keyboard path in this commit is verified by unit test and by
the console equivalents, **not** by a physical keypress. That gap is stated plainly rather
than papered over — the palette's *drawing* is screenshot-verified, its *ranking* and its
*adjustment* are test- and console-verified, and its *key bindings* are read-only-reviewed.
`Escape()` is untouched, per the brief.

### D15 · Ten calls taken while building `Kind::Composite` and migrating Monitor and Log, P3 part C

P3 part C replaces the last two `Escape()` hatches (`system.monitor`, `system.log`) and
builds the composite band D14.10 deferred. **`EscapeCount()` 2 → 0: no area is escaped.**
Build clean, **67/67 tests** (`overlay_ui` 45 → 47, `overlay_atoms` 6 → 8). Full write-up:
`round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D15.1 · `Kind::Composite` is drawn by a function that is DELIBERATELY a near-copy of
DrawEntryRow, not by a special case inside it.** `DrawCompositeBand()` repeats the selection
fill, the hairline, the label/value split and the disabled handling rather than sharing them
through a flag. That looks like duplication and is the point: SPEC §4.2 clause 2 says "line 1
reads as a row", and the cheapest way to guarantee that forever is for line 1 to be built by
code that is *shaped* like a row, from an ordinary `RowCtx` handed out by `Band.cpp`.
**Rejected:** threading `nLines` through `DrawEntryRow` — every allocator inside it would then
need to know whether it was in a band, which is exactly how the two legacy Position Grid call
sites drifted apart. `LinesFor()` is the single answer to "how tall is this declaration", so
the sheet and the Inspector cannot disagree.

**D15.2 · The band's hit box covers LINE 1 ONLY, not the whole band.** The obvious version
puts one `InvisibleButton` over the full `n × 44` and selects the row from anywhere in it —
and swallows every click meant for the 3×3 grid, which lives inside those same bounds. So the
selector covers the region left of the body, and the body owns the rest. **The visible cost:**
clicking the air on lines 2..n does nothing rather than selecting the row. That is the correct
trade — a grid you cannot click is a control that renders and does nothing (#25, #68), and a
strip of dead air is not.

**D15.3 · A composite reads and resets BOTH its axes, and that is a registry change, not a
renderer one.** An anchor is one setting whose value is a pair. `HasDefault`/`IsAtDefault`/
`ResetToDefault` previously read `m_Bind` alone, so an anchor at the right column but the wrong
row reported itself unchanged and the half that had moved could never be reset. Since D6's
accent edge is driven off `IsAtDefault()`, a single-axis implementation would have made the
sheet *lie*. Fixed in `Registry.cpp` with a test, rather than worked around in the band.

**D15.4 · The Colour override composite is now REGISTERED, which SPEC §4.4 explicitly required
before claiming it exists.** SPEC lists five composites and notes Colour override is
"documented here but not registered in the mockup… do not re-add 'five composites' language
without also adding the registration." Issue #29's four per-module colours are that
registration. **The config format is untouched** (`std::optional<int>`, packed `0xRRGGBB`,
`nullopt` meaning "track the accent token"); the control edits OKLCH and converts back on every
edit, which is why `palette::ImU32ToOklch()` was added next to its forward twin with a
round-trip test pinning the two together. **Rejected:** a hex `Text` param, which preserves the
capability with zero new machinery but replaces a colour picker with typing.

**D15.5 · The colour band is never disabled when its override is off — because `custom` is its
own Param.** The natural shape greys the band while the override is off. SPEC §3.13's
inheritance would then grey the very param that causes the greying, which that section names as
a real bug from the first version. So the band always shows the colour in effect and *editing*
it turns the override on; `custom` turns it back off and is always reachable. The band also
declares **no default of its own**: a default packed colour would have to be captured at
registration, and the token it comes from moves with the accent hue, so D6's edge would light
on every colour row the moment someone rotated the hue.

**D15.6 · Issue #40's collection gating is now a SWITCH, not a side effect of navigation.**
#40 tied 60-second history collection to tab selection; there is no tab bar any more. Stating
it outright is also more honest — the old shape made a background cost invisible. **The config
key and both string values are unchanged** (`overlay.system_monitor_tab`, `"statistics"` /
`"modules"`), it is still written the instant it changes rather than batched (that field gates
`FpsDisplay_AddLayer()`, and routing it through the debounced path silently dropped every
change during #59's own manual testing), and a restart still resumes collecting.

**D15.7 · `Area::Content()` is a new capability, and it is NOT `Escape()` renamed.** The Log is
the one area whose body is content rather than settings. `Escape()` handed a call site the
sheet's child window and let it run arbitrary ImGui with every law suspended — which is why it
was always temporary. `Content()` hands the shell **data**: a function returning lines. The
call site still cannot place a pixel, pick a font, a colour or a width, or lay anything out
(SPEC §5.2 clause 0 holds). A content area still declares ordinary rows — Log's filters go
through the same grammar, Inspector and help — so an area is never "rows or content", it is
rows **and**, optionally, content beneath them.

**D15.8 · The Log's filter rows are a vertical group, not the mockup's horizontal filter bar.**
`index.html` renders `log.sources`/`log.severity`/`log.filter` as a bar above the body, from
the registry, "same painter, different host". **Chose** ordinary rows in a `Filter` group
instead. **Why:** a horizontal control bar is a second layout system inside the sheet, and the
Row grammar's whole guarantee is that there is one — SPEC §5.3 forbids exactly this for the
Inspector, and the argument does not weaken for the sheet. Nothing is lost functionally; every
filter keeps its label, help, Inspector and reset. Reversible if the bar is wanted later, but
it should be a deliberate second host, not a drive-by.

**D15.9 · Issue #81's Copy button ships DISABLED, with the reason stated on the row.** It never
reached the system clipboard: no clipboard handler is wired for the overlay's ImGui context, so
with `io.SetClipboardTextFn` unset ImGui falls back to an internal buffer nothing outside the
process can read. **Wiring it is a feature, not a fix** — gamescope *is* the compositor, so a
real implementation offers a `wl_data_source` selection on its own seat, and must decide what
"the system clipboard" means when running nested and the host session owns one too. Disabled
with a mandatory reason beats a button that lies; "it silently did nothing" is strictly worse
than "it told you it cannot".

**D15.10 · Two defects were found by running the thing, and both are recorded honestly.**
*(a)* `Kind::Bank` and `Kind::Text` **drew nothing at all** — `DrawEntryRow`'s switch let both
fall to `default: break`. Both atoms have existed since P1 with tests; no area had used either
kind until the Log, so the taxonomy claimed eleven kinds while the shell rendered nine. That is
#25/#68 produced by an unhandled enumerator. *(b)* `overlay_e2_set overlay.display_scale 2.0`
**aborted the compositor** on an ImGui font assertion. `fonts::RebuildAll()` mutates font state
and assumes it is called from the render thread inside a live frame; a registration setter is
reachable from the **console thread**, so it cleared an atlas mid-draw. **Checked rather than
assumed:** the same command was run against the P3b merge, which survives it — so the faulty
call is P3b's and this work merely made it deterministic (a band's value string needs a glyph
baked at the new size on the very next frame). Fixed by deferring the rebuild to the render
thread.

**Still open, and NOT fixed here.** `overlay_e2_set overlay.display_scale` updates the stored
value and re-bakes the atlas, but the shell's rendered geometry does not change — the slab
stays 1560 px wide where `Slab::For()` says 2.0× should give 1728. It is P3b's row and outside
this part's scope, and one out-of-scope crash on that same row was already fixed here. The
band's geometry at 2.0× is instead pinned by test (`band: the four clauses hold at every
display scale`), which is the property the screenshot was meant to establish.

### D14 · Eleven calls taken while migrating Audio and Config, P3 part B

P3 part B replaces the `Escape()` hatches on `audio.mixer` and `setup.config`, and fixes the
Inspector scroll defect P3a found and left. `EscapeCount()` 4 → 2. Build clean, **67/67
tests** (`overlay_ui` 34 → 45, `overlay_shell` 16 → 19, `config` 38 → 40). Full write-up:
`round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D14.1 · The Inspector scrolls by moving its ORIGIN, not by gaining a second layout path.**
The bodies lay out with an absolute `y` painted onto the draw list. The bug was never that
grammar — it was that `y` started at the *region's* `y0`, a fixed screen coordinate, and that
nothing told ImGui how tall the result was. Both halves are fixed in the body child: the
origin is now the **child's own cursor** (from which ImGui has already subtracted the scroll
offset, so the existing absolute arithmetic pans correctly), and each body returns its bottom
edge, which becomes a `Dummy` and therefore a scroll range. **Rejected:** rewriting the bodies
onto ImGui's cursor with `Dummy`/`SameLine` spacing — that leaves one layout model in the
sheet and another in the Inspector, which is exactly what SPEC §5.3 forbids, since a promoted
parameter must land in the sheet unchanged. *Note how thin the margin was:* at 2.0× the six
param rows came to 736.0 px against a 736.8 px body, which is why only the **last** row
clipped rather than the whole block.

**D14.2 · Dynamic rows get an identity from the STREAM, never from a position — and that is
why the registry changed rather than the audio file.** The cheap workaround is a fixed pool of
N slots, each bound to "whichever stream is at index *i*", greyed when there are fewer. It
fits every existing law with **zero** registry changes. **Rejected anyway**, because slot
identity would be positional: a stream ending shifts every stream below it up one slot, so the
slider under the pointer silently starts controlling a different application. That is not a
cosmetic problem — it is the volume of the wrong program moving. **Chose:**
`ui::Area::Rebuilds( generation, builder )`, with ids of the form
`audio.node.<pipewire-node-id>`. This is P1's deferred `Area::Repeat()` finally landing; P1
said it "needs the shell's frame loop to have a topology-change hook", and
`Registry::SyncDynamicAreas()` at the top of `Draw()` is that hook.

**D14.3 · The law a dynamic area breaks is HELP IS REQUIRED, and it is closed rather than
waived.** Id uniqueness survives because a rebuild *releases* its ids before it builds; the
Prefix Law and the Six Budget are untouched because a rebuilt Entry goes through the same
synthesis and the same per-Entry check. But `Registry::SelfTest()` runs **once**, after
`RegisterAll()`, so a row built later had never been through it — a generated row could ship
with no `Help()` at all. `SyncIfStale()` now re-runs the help and prefix checks over its own
area. **The cost, stated plainly:** a violation aborts (D11.6) and a rebuild happens
mid-session, so a malformed dynamic row is a **mid-session abort**, not a boot failure. That is
a real widening of when the guillotine can fall, and it is acceptable only because a dynamic
row is *generated* — no human types one — so one test against a fabricated stream list covers
every row the generator will ever emit. **The empty stream set has its own test**, because it
is the common case at startup and is precisely when the row-building code does *not* run.

**D14.4 · Issue #63's name precedence is CONSUMED, never re-derived.** `StreamCandidate.sLabel`
already *is* the `application.name` → `media.name` → `wpctl`-label chain, assembled in
`Volume.cpp` from ncpamixer's source. The area reads it and adds nothing. **Why it matters
enough to record:** a second copy of that precedence would be a second answer to the same
question, and the tier that actually rescues the hard case (a stream with **no**
`application.name` at all) is the middle one — verified live, where such a stream resolved to
*"iPhone von Moritz (codec AAC)"* rather than its raw `bluez_input.AA_BB_…` node name.

**D14.5 · Issue #36's jump-back fix is REUSED, not reimplemented.** Every volume and mute
binding goes through the existing `ResolveDisplayVolume()` / `ResolveDisplayMute()` optimistic-
pending layer. Audio's poll thread is on a 750 ms cadence, so a naive read between letting go
of a slider and the next tick still returns the old value and the slider snaps back. Binding
straight to `candidate.flVolume` would have reintroduced exactly the bug the brief warned
about.

**D14.6 · The Config panel's three tabs became THREE AREAS, on D13.1's precedent.** `setup.config`
is gone; `setup.profiles`, `setup.pergame` and `setup.appearance` replace it, which is what
`index.html` — the declared tiebreaker — lists as rail items. The rail is now the **eleven**
items SPEC §8.1 names, which is also the count D12.8 deferred eleven icons for. *Cost:*
`overlay_e2_select setup.config` no longer resolves.

**D14.7 · The layer badge is an AREA property, not a row one.** It answers issue #43's
question — *where does what I change here get written?* — with `global`, `app <id>` or `global
only`, right-aligned in the sheet header. **Why per-area:** it describes the file a whole sheet
routes to, and the awkward case cannot be derived from session state at all — Appearance writes
`global.json` even for a game with an override active, because `overlay.*` is process-level.
That is the same rule that forced the old title bar to take a per-tab override string and
accept a one-frame lag; a derived per-area badge has neither.

**D14.8 · Reset was MISSING, and this is where it landed.** D6 decided reset moves into the
Inspector, but no phase implemented it — so the E2 shell could not reset anything, and
migrating Config would have silently dropped its four per-group links. **Chose:** per-row
reset that **also covers the row's parameters**, which is what makes it the successor to a
*group* link rather than something weaker — the old "UI Scale" group *is* the `UI scale` row
plus its dock and notification params. A row with no declared `Default` shows no affordance
rather than resetting to zero, and float comparison uses a tolerance so a config that had
merely been saved and reloaded does not light the link forever. **Still open:** D6's other
half, the accent left edge marking "differs from default" on the sheet.

**D14.9 · A destructive action is armed by its DECLARATION.** The user, after an agent wiped
one of their configs: *"There can be a button for it, but never delete configs
automatically."* `Entry::Confirm( prompt )` makes the first press swap the verb and redden the
chip, and only the second perform it; it disarms on a timeout, so an overlay someone walked
away from is never one click from destroying a file. **Rejected:** a modal the call site
opens. A confirmation a call site must remember to build is one the next call site forgets —
and a category file cannot place a pixel at all (SPEC §5.2 clause 0), so it could not open one
anyway. `config.delete` is the only action in the product that destroys anything, and it
exists only when there is a saved config to destroy.

**D14.10 · Two affordances lost fidelity, both because the kit cannot yet express them.** The
accent hue's **gradient strip and live swatch** are not carried over — the hue *setting* is,
as a slider with a degree unit. The notification **3×3 placement grid** is one nine-option
Choice. **Why not the grid:** `ui::Kind::Composite` is declared in the taxonomy and
`controls::AnchorGrid()` exists, but the shell does not render a composite at all — it falls
through to `default: break`. An Anchor here would be a control that registers correctly and
draws nothing, which is precisely issues #25 and #68. A nine-option Choice is still **one
setting with one value**, which is what issue #27 was actually about when it replaced two
independent segmented controls with the grid. Both return when Composite lands.

**D14.11 · `overlay_e2_scroll`, and a crash the console path found.** Scrolling is a pointer
gesture and pointer injection is permanently forbidden here (D4), so a third console command
follows D12.7 and D13.8 — without it the scroll fix is unverifiable by anything but a hand. It
is a *request*, pushed into ImGui only on the frame it changes, so the wheel stays
authoritative. Separately, `overlay_e2_set` earned its keep again: a registration's setter is
reachable from the **console thread**, which has no ImGui context, and `PushLiveTheme()` wrote
`ImGui::GetIO()` unguarded — `overlay_e2_set overlay.display_scale 1.0` killed the compositor.
Guarded now; that line is purely the live drag preview, and a console write is not a drag. The
general lesson is worth more than the fix: **any** binding that touches ImGui state is now
reachable off the draw thread.

### D13 · Nine calls taken while migrating Display and Shaders, P3 part A

P3 part A replaces the `Escape()` hatches on `display.gamescope` and `image.shaders` with real
registrations. `EscapeCount()` 6 → 4. Build clean, **67/67 tests** (`overlay_ui` 30 → 34 cases).
Full write-up: `round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D13.1 · The four Gamescope tabs became three AREAS, not one area with four groups.**
`display.gamescope` is gone; `display.upscaling`, `display.frame_limiter` and `display.hdr`
replace it. **Rejected:** keeping one `display.gamescope` area with four group bands — the
smaller, more reversible change. **Why:** the rail is the product's only navigation (SPEC §8.1
lists exactly these as rail items, and `index.html` — the declared tiebreaker — declares them as
separate areas), and a four-group sheet is a tab bar redrawn as headings. The old **Display** tab
has no successor on purpose: its three settings are two presentation ones (tearing, cursor grab)
and one refresh one (VRR), so they joined the areas that already own those concerns rather than
forming a fourth area with no theme. *Cost, stated plainly:* the rail grew from 7 items to 9, and
`overlay_e2_select display.gamescope` no longer resolves.

**D13.2 · `display.output` from the mockup was NOT created.** The mockup's fourth Display area
holds resolution, refresh rate, colour range and rotation. **Why not:** gamescope-ritz has no
config keys and no setters for any of them. Building the area would mean inventing four settings,
and the brief's hardest rule is that this is a presentation change. The mockup is the tiebreaker
for *how a thing should look*, not a list of features to add.

**D13.3 · The frame limiter is ONE Stepper, not a toggle plus a slider.** Issue #67's valid set is
0 **or** [10, 480], with a real hole — 1-9 fps is a trap, because at that rate the overlay itself
is too slow to drive, including too slow to undo. The legacy tab spent two controls on that hole
(an "Unlimited" switch plus a 10-480 slider) because `widgets::SliderInt` cannot express a gap.
**Chose:** `Stepper.Range(0, 480).Step(10).ZeroMeans("Unlimited")`. With a step of 10 anchored at
0 the reachable set *is* {0, 10, 20, …}: the hole is a consequence of the step, not a special case
anyone maintains, and one setting is one row again. `SetFpsLimit()` still clamps every write, so
the floor holds for the ConCommand and `gamescope_control` paths too. *Cost:* values that are not
multiples of 10 are no longer reachable **by stepping** (an existing 144 loads, displays and works;
stepping from it moves to 154). Nothing on disk changed.

**D13.4 · Adaptive brightness sits on exactly six params, and that was left visible rather than
worked around.** Six is the budget; a seventh aborts registration. It fits with zero headroom.
**Chose:** declare all six, and say in the code and in the Inspector (`PARAMETERS 6 of 6`) that
the next one is a design signal, not an obstacle — the effect would have become a *category*.
**Rejected:** pre-emptively promoting it to its own area now, or demoting a param to make room.

**D13.5 · Three settings that were prose in the old panels became `.Live()` facts, not rows.**
The Upscaling tab's orange "Steam is focused — filter/scaler forced to Fit/Linear" banner, the HDR
tab's read-only app-metadata strip, and the HDR tab's "Tonemap Operator: deferred" note are all
statements about live state, not settings. **Chose:** each is a readout on that area's Diagnostics
`Facts` row. **Why:** a `Facts` row cannot be given a control at all (`.Live()` has no `Bind`
overload), which is a stronger guarantee than the legacy "never editable here" comment was — and
the tonemap note in particular records *why there is no control*, which is the one thing dropping
it would have lost.

**D13.6 · The disabled dim moved under `Col()`/`Accent()` as `ui::ScopedDim`.** SPEC §3.13 says
"row × 0.55", meaning label, value **and** control. The control atoms paint straight onto the draw
list with token colours, so ImGui's `BeginDisabled()` alpha never reaches them — the first
implementation dimmed only the text, and HDR's greyed sliders stayed fully lit. **Rejected:**
giving every atom a `bool bDisabled` parameter; the first atom that forgot it would be a control
that greys everywhere except where it matters. The factor now lives under the two functions every
pixel already goes through, so a new atom is dimmed correctly before it is written.

**D13.7 · The downgraded Choice got its dropdown implemented.** `controls::Choice()` auto-
downgrades to a dropdown when the segmented group does not fit its lane, and P2's shell ignored
`bWantsPopup` — it had one Choice with three short options and never saw the downgrade. A
five-option filter row in a narrow sheet does. **Why this mattered enough to do now:** without it
the control renders correctly and does nothing, which is precisely the shape of issues #25 and
#68.

**D13.8 · A new console command, `overlay_e2_set <id> <value>`.** It writes through
`Entry::Binding().Set()` — the same call a click makes, not a parallel path. **Why:** pointer
injection is permanently forbidden here (D4), and no keyboard-injection tool is installed either,
so "does this row actually drive the compositor, or does it merely render?" had **no** answer
available to a script. That is not academic — #25 and #68 were both controls that rendered
correctly while doing nothing, and both passed review. This is the same argument that produced
`overlay_e2_select` in D12, applied to a registration's value instead of its selection. It can
only address registered ids and refuses a read-only kind.

**D13.9 · The legacy panels were left completely intact.** `PanelDisplay_Draw()` and
`PanelShaders_Draw()` and every `DrawXxxTab()` still exist and still run under `overlay_e2 0`.
Only the two `Panel*_DrawBody()` escape hatches were deleted. **Why:** D10 says the two UIs
coexist behind the ConVar until the default flips at the end of the phase; deleting the legacy
body now would make `overlay_e2 0` a worse UI than it is today, mid-phase.

### D12 · Eleven calls taken while building P2, the shell

P2 is the three-region shell — `src/Overlay/UI/Layout.*` (pure geometry) and `Shell.*` (the
ImGui half) — behind the `overlay_e2` ConVar, hosting the five existing panels plus
FpsDisplay's settings half verbatim. Build clean, **67/67 tests** (P1's 66 plus
`overlay_shell`). Full write-up: `round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D12.1 · The ConVar is `overlay_e2`, not `overlay.e2`.** The task brief and the phase plan
both name it with a dot. **Why the underscore:** every one of gamescope's ~200 ConVars is
snake_case, and a single dotted name would be the only one in the system — a reader
grepping `cv_` finds it, a reader grepping the docs' spelling does not. The concept,
default and gating are exactly as specified. *Cheap to reverse:* one string literal.

**D12.2 · The ConVar is runtime-only; no config field, no on-disk key.** **Rejected:**
adding `overlay.e2` to `OverlaySettings` so a user could persist the choice. **Why:** the
brief's hardest rule is that no on-disk key or format changes this phase, and a schema field
is exactly that. Off-by-default plus opt-in-per-session is also the correct shape for a
half-built UI: a persisted flag would survive a restart into whatever state P3 left the
shell in. The persistence question belongs to the phase that flips the default.

**D12.3 · One genuinely-E2 area (`setup.shell`) ships alongside the six escaped ones.**
**Rejected:** escaping all seven and leaving the Inspector permanently on Overview. **Why:**
SPEC §5.2 clause 0 makes the Inspector a pure function of a *registration*, and an escaped
area has no entries by construction — so with only escaped areas there is no selection
anywhere in the product, and CONFIGURE and DETAILS would both ship as untested dead code in
the phase whose job is to prove the shell holds them. `setup.shell` describes the shell
itself: a Choice with one Param (Configure), a Facts row with three `.Live()` readouts
(Details), and an Action. Every binding is to shell runtime state or to a ConVar — **never
to a config field** — so D12.2's rule is not bent to satisfy this one.

**D12.4 · Escape() is sheet-only, and an area is legacy or E2, never both.** `API.md` §13
describes the hatch but not its limits. **Chose:** two, both enforced by
`Law::Escaped`. (a) No Inspector equivalent — SPEC §5.2 clause 0 says the Inspector has no
authoring API, and a hatch into it would be the fifth generator that clause forbids; an
escaped area therefore shows Overview and nothing else. (b) Escaping a populated area and
populating an escaped one both fire. **Why (b):** the half-migrated area — three real rows
plus an escaped tail — is the shape that mostly works and therefore never gets finished,
which is how a temporary hatch becomes permanent. `Registry::EscapeCount()` is the number a
future `ui_lint` reports as severity `migration`.

**D12.5 · The panels were split, not copied.** Each `Panel*_Draw()` became a file-static
`DrawBodyContent()` plus a thin window wrapper, with a new public `Panel*_DrawBody()`.
**Rejected:** a second drawing path per panel for the E2 sheet. **Why:** two copies of a
panel body would both have to be maintained through P3, and they would drift. The legacy
path is unchanged; the one behavioural difference is `Audio::GetState()` moving inside the
body (a pure read under a mutex).

**D12.6 · `chrome::EnsureLiveThemeLoaded()` was exported rather than duplicated.** It is the
one-shot that pulls `display_scale` and the accent hue out of `global.json`, and it has only
ever been reachable from `BeginPanelWindow()` and `DrawDock()` — issue #79. The E2 shell
draws neither, so under `overlay_e2 1` the trap would have been *permanent* rather than
first-frame: the shell would render at 1.0× for the life of the process. **Chose:** a
`chrome::EnsureThemeLoaded()` forwarder the shell calls once per frame. **Rejected:**
re-reading `global.json` in the shell (two loaders, two answers) and moving the loader out
of Chrome.cpp (a bigger refactor than P2 should carry).

**D12.7 · Two console commands, `overlay_e2_host` and `overlay_e2_select`.** Not in the
spec. **Why:** the shell's state — which area, which row, which host — was otherwise
addressable only by pointer, and pointer injection is permanently forbidden (D4). Without
them P2 could not be verified at all, and a bug report could not say which host it was in.
They reach exactly the three things Ctrl+I and a click reach, they are not persisted, and
they are the project's own idiom rather than a new one. The host preference now has **one**
storage — the ConVar itself — so the console, Ctrl+I, the spine, the close glyph and the
`setup.shell` row cannot disagree.

**D12.8 · The rail draws initials, not SPEC §8.0's eleven icons.** **Why:** eleven stroked
24-unit glyphs are a self-contained piece of work with their own acceptance criteria (one
recognisable silhouette at 12 px, no new detail at 48 px), and they belong with the area
rewrites that name them. P2's job is that the region is the right width and the active item
carries its accent edge through the collapse — both of which are visible and testable with a
placeholder glyph.

**D12.9 · The icon rail's collapse is decided by the ladder, never by the animated width.**
Found by screenshot: `DrawRail` re-derived "is this the icon rail" from the width it was
handed, and that width is animated. `Approach()` is an exponential and never lands exactly
on 60, so the rail was permanently a fraction too wide to count as collapsed and drew full
labels into a 60-wide strip. **The general rule this encodes:** a presentation value must
not make a semantic decision. The animation now also snaps within a physical pixel so it
terminates.

**D12.10 · The inspector is one child window, because that is what buys z-order.** The
drawer initially rendered *behind* the sheet: its background was painted onto the slab's
draw list, and ImGui renders every child after its parent's own commands. **Chose:** put the
whole region in a child begun after the sheet's, which puts it above by the same rule.
**Rejected:** `ImDrawListSplitter` channels, which would have made the region's paint order
a thing every future edit has to know about.

**D12.11 · Six type roles map onto five baked font styles, and that is left alone.**
`Fonts.cpp` bakes what the *legacy* design guide needed; SPEC §7.6 wants six. A sixth style
is an atlas change, which is a rebuild, which is issue #51's territory. **Why defer:** a
role landing one style off costs half a point of size, not a layout, and P3 does the
typography pass anyway. Recorded so it is not mistaken for an oversight.

### D11 · Ten calls taken while building P1, the kit foundation

P1 is `src/Overlay/UI/` — tokens, the registry and its laws, the row grammar, the control
atoms, and two test suites. Build clean, **66/66 tests** (the 64 that existed, plus
`overlay_ui` and `overlay_atoms`). Full write-up:
`round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D11.1 · `src/Overlay/UI/`, not `src/Overlay/Kit/`.** The task brief offered `Kit/` as an
example; `API.md`'s own "Proposed location" section names `src/Overlay/UI/` *and lists the
filenames*. **Why:** the contract already spells the paths, and picking a different one
would make every path reference in `API.md` wrong before a line shipped.

**D11.2 · `SPEC.md` §2.2's lane clamp is a transcription slip; the bounds are literal.**
Both `SPEC.md` §2.2 and `API.md` §6 print `Lw = clamp( round(0.46 × W), W − 420, W − 200 )`.
Those bounds cannot be right: §8.3 works the same formula at W = 804 and states
`clamp(370, 384, 604) = 370`, which is false — 370 is below its own lower bound. Read as
**literal widths of the label+value zone**, `clamp( round(0.46 × W), 200, 420 )`, every
number in §8.3 comes out exactly (Lw 370, label 358, control 394, and the columns sum to
804), and §2.2's own prose about a "470-wide" control zone at the 1.0× sheet width is
reproduced too (468). **Chose** the literal reading. **Rejected** implementing the printed
bounds, which would put the control zone at 380 and contradict §8.3 and §2.2 both.
*Cheap to reverse:* it is one clamp in `Lane.cpp`, and two tests pin the numbers.

**D11.3 · The C++ class is `ui::Parameter`, not `ui::Param`.** C++ forbids a member function
whose name matches its enclosing class, and `API.md` §7's chained `.Param()` declarations
need exactly that member. **Why not rename the method instead:** the method name is what
appears in every category file for the rest of the project; the class name appears almost
nowhere. Rename the cheaper one.

**D11.4 · `Parameter::Param()` is a *sibling* factory.** `API.md` §4.2 says a Param "has no
`.Param()`", but §7 and §8 both chain one off a Param. Resolved by making it forward to the
owning `Entry` and return another child of that same Entry. **Why this keeps the law:** the
id is *always* synthesised from the Entry, so there is no operation on any type that
produces a Param owned by a Param. One Level survives as a type-level guarantee; only the
spelling was reconciled.

**D11.5 · The value type role is 16, not `SPEC.md` §7.6's 15.** §7.6's table says Mono 500
15; §2.3 says the value is "B's `.val` size", and the mockup draws `.val`, `.dd .tv` and
`.tin .tv` all at 16. **Why:** the brief makes the mockup the tiebreaker, and two of the
three sources already say 16.

**D11.6 · A law violation aborts, with a test-only recorder as the single seam.** The
project builds `-fno-exceptions`, so a violation cannot be thrown. **Chose:** print and
`abort()` — a malformed registry is a boot failure, since `RegisterAll()` runs once at
startup. `ui::LawRecorder` changes only *what happens after* a violation fires, never
whether it fires, so the tests can assert which law caught what without killing the binary.
**Rejected:** returning an error code (nobody would check it) and logging a warning (that
is a convention, which is the exact thing §5.2 exists to replace).

**D11.7 · The kit owns its own copy of `display_scale`.** `ui::SetScale()` / `ui::Scale()`,
seeded by the shell once per frame, instead of every file calling
`palette::DisplayScale()`. **Why:** that accessor reads `palette::g_LiveTheme`, which is
*defined in `Chrome.cpp`* — binding the kit to it would drag 1,600 lines of legacy dock
machinery into anything that wants a row height, including the tests. One reader of
display_scale in the whole kit is also just better design.

**D11.8 · Band geometry lives in its own file, not in `Controls.cpp`.** `API.md` puts it in
`Composite.cpp`; it is here as `Band.h/.cpp`. **Why:** a band is row *grammar*, not a
control atom, and keeping it free of ImGui is what makes SPEC §4.2's four clauses directly
testable.

**D11.9 · The stepper's repeat uses ImGui's own timing, not SPEC §3.5's stated 400 ms.**
`ImGuiItemFlags_ButtonRepeat` and `io.KeyRepeatDelay` (0.275 s by default). **Why:** every
other behaviour in the atoms is stock on purpose; introducing a hand-rolled timer to hit one
number would be the only bespoke input path in the kit. Trivially changed later by setting
the io field if the difference is ever felt.

**D11.10 · Six things are deferred, each for a stated reason** — the dropdown popup and
text validation (the mockup itself calls the popup "shell furniture"), `Repeat()`, `Cfg()`,
`Escape()`, and `ui_lint`/`ui_snapshot`. All five need either the shell (P2) or a populated
registry (P3) to attach to. Listed with reasoning in `IMPLEMENTATION.md`.

### D10 · The implementation is phased, coexists with the old UI behind a ConVar, and only flips at the end

**Baseline before starting:** master at `c108ee4`, build clean, **64/64 tests**, 7,713 lines in the
files the rework touches (`Chrome.cpp` 1627, `Widgets.cpp` 942, `FpsDisplay.cpp` 2469, the five
`Panel*.cpp` 2675).

**Chose** five phases, each independently buildable and testable, with the new UI behind a ConVar
so both exist side by side until the last phase:

| | Phase | Ships |
|---|---|---|
| **P1** | Kit foundation | tokens, registry, row grammar, control atoms — new files, nothing wired up |
| **P2** | Shell | rail / sheet / inspector regions, behind `overlay.e2`, initially hosting existing panels verbatim |
| **P3** | Areas | one area per commit: Display, Shaders, Audio, Config, Monitor, Log |
| **P4** | Palette | registry-driven search, `Ctrl+K` |
| **P5** | Removal | delete dock, floating windows and dead widget paths; flip the default |

**Rejected:** a single big-bang replacement, and porting panels before the kit exists.

**Why the ConVar matters more than it looks.** It means every phase is shippable and reversible,
the two UIs can be compared directly on the same build, and a phase that turns out wrong costs one
revert rather than a rewrite. It also lets the user see progress mid-flight rather than only at the
end. E2's own migration plan reached the same conclusion independently — its PR 1 hosts existing
panels inside the new shell.

**Why one area per commit in P3.** These panels have already lost features to a careless merge
once (#29's colour controls nearly vanished when #40 restructured the same file). One area per
commit keeps each diff reviewable and each regression bisectable.

**Two constraints that hold throughout**, both from rules this user has stated repeatedly:
- **64/64 tests stay green at every commit.** Config round-tripping is already covered by tests
  (`tests/test_config.cpp`); the rework must not weaken that.
- **Existing configs keep loading, and nothing is silently rewritten.** If the registry changes how
  a setting is addressed, the on-disk key does not change with it.

**The FpsDisplay caveat:** it is 2,469 lines and does two jobs — the HUD drawn over the game, and
the System Monitor settings panel. **Only the settings half moves.** The HUD is not part of this
redesign and must come out of P3 behaving exactly as it does today.

### D5–D9 · The five open questions from `INCONSISTENCIES.md` §F

The mockup agent deliberately left five design calls rather than choosing silently. All five are
answered below. **Each is cheap to reverse** — none has been implemented in C++ yet.

#### D5 · Area ids stay as they are; the docs state they are UI grouping only

Four of eleven areas have a rail id that doesn't match their settings' key prefix
(`system.monitor` holds `monitor.*`, `setup.pergame` holds `config.*`, and so on).

**Chose:** document that **area ids are UI grouping and carry no promise about key prefixes**,
and change the palette's path column to show the *config key* rather than the area id, which
removes the only place the mismatch is visible.

**Rejected:** renaming the config keys to match the rail.

**Why:** the keys are on disk in the user's `global.json` and `games/*.json`. Renaming them is a
migration, and this user has been emphatic that existing configs must keep loading and must never
be silently rewritten — it is the one rule they have repeated. A cosmetic mismatch in one column
is not worth a migration, and showing the real key is more useful there anyway.

#### D6 · One encoding of "differs from default" on the sheet, and the reset action moves to the Inspector

Three encodings currently coexist: a 2px accent left edge, an accent-coloured value, and a reset
dot. The dot *displaces* the depth chevron, so a row that both differs and has depth stops
advertising its depth — a functional regression, not merely redundancy.

**Chose:** keep the **accent left edge**; drop the value recolour; move **reset into the
Inspector**.

**Why:** the edge is the only one of the three that scans vertically, which is exactly what "what
have I changed here" needs. The value recolour competes with the accent's other job (live or
active state). And reset is a deliberate action, so the Inspector is where it belongs — that is
E2's whole thesis. This also frees the affordance column, so depth always advertises itself.

**Cost, stated plainly:** resetting now takes selecting the row first. One extra click for a rare,
destructive-ish action, in exchange for depth never being hidden.

#### D7 · Keep the chip bank as a control kind

**Chose:** keep it, under the agent's stated rule — *a bank is one setting whose value is a set;
N independent binaries are still N switch rows*.

**Rejected:** collapsing `log.sources` and `log.severity` to dropdowns.

**Why:** it expresses a set-valued setting, which no other control in the taxonomy does, and a
dropdown actively misleads by implying a single choice. The rule is what stops it becoming a
checkbox by another name, so the no-checkbox law survives.

#### D8 · Fold the single-area sections

**Chose:** `IMAGE` folds into `DISPLAY`, `AUDIO` folds into `SYSTEM`.

**Why:** a section header that groups exactly one item buys nothing at 1.0× and costs a line at
0.5×, where vertical space is tightest. If a second area appears later, the section comes back
then. Trivially reversible either way.

#### D9 · The dev chips are mockup-only, and that is written down

The sheet header's `text lines`, `row heights`, `col`, `ladder` and `sheet 804b` chips are
instrumentation sitting inside product chrome.

**Chose:** keep them in the mockup — they are the evidence for the density argument — and record
in `SPEC.md` that they are **mockup instrumentation and must not be implemented**, plus carry it
as an explicit item on the implementation checklist.

**Why:** the risk isn't the chips, it's a later reader treating the mockup as pixel-exact and
porting them. Saying so in the spec is cheaper than remembering.

### D1 · Implementation goes on `feature/overlay-e2` — a branch, never master

**Chose:** a long-lived feature branch off `master`, with the work merged into it in reviewable
steps; `master` untouched until the user has seen the result.

**Alternative:** committing to `master` as usual, which is how every fix today has been handled.

**Why:** the user explicitly said *"This is fine, since you are working on another branch."* That
is both permission to act and a constraint on where. A UI replacement is not a bug fix — it
touches every panel and cannot be judged from a diff, so it must be trivially abandonable.

### D2 · The mockup is tested before any C++ is written

**Chose:** treat the HTML mockup as the artefact under test first — drive every control, every
area, every scale, every state — and fix defects there before implementing.

**Alternative:** start the C++ port immediately and discover mockup defects through the
implementation.

**Why:** the user asked for exactly this ordering ("Once the updated UI comes back, test it
thoroughly... Once the design is complete, create a new branch"). It is also far cheaper: a
layout mistake costs minutes in HTML and hours in ImGui. The mockup is explicitly the base we
compare the implementation against, so it has to be right first.

### D3 · Testing the mockup uses a headless browser, not the eye

**Chose:** automated DOM-level checks — every control reachable and operable, state coherent
after interaction, no console errors, all areas populated, every scale step rendering — plus
screenshots for judgement calls.

**Alternative:** manual visual review only.

**Why:** "thoroughly" over ~11 areas × several scales × two inspector modes is thousands of
states; sampling by eye reliably misses the sparse ones, which is exactly where today's audit
expects inconsistency to hide. Pixel measurement also beats impression — that lesson is already
recorded in issue #52, where a false bug report came from a distorted screenshot.

### D4 · No pointer injection, permanently

**Chose:** no `ydotool` or synthetic pointer input while implementing or testing, even though
the user is asleep and would not be disturbed.

**Alternative:** use it freely overnight, since nobody is at the machine.

**Why:** agents have twice injected clicks that landed in the user's browser, once because a
test window died and once because gamescope launched on the wrong monitor. Absence is not a
safeguard — the second incident happened while they were away from the keyboard. Verification
uses config presets, ConVars, `grim` screenshots and keyboard only.

