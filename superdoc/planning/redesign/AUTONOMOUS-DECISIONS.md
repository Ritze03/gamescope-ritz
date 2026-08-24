# Decisions taken without the user

The user went to sleep on **2026-08-23** having said: *"I will go to sleep now, so you will have
to make decisions. This is fine, since you are working on another branch. Write down the
decisions, that you made, so i can confirm that they are fine later!"*

Everything below was decided in their absence and **needs their confirmation**. Newest block on
top. Each entry records what was chosen, what the alternative was, and why — so disagreeing is
cheap.

---

## 2026-08-24 — D30 · The type ladder rises a second time, and `Meta` moves most

The user, one round after D23: *"Make the Log and Changelog font 1-2px bigger. In fact, we
should increase all of the smaller font sizes by 1-2px."* Directional again, not a table,
so the six numbers were judgment again — and this time the numbers had to explain why the
first pass didn't land.

### D30.1 · `Meta` gets the biggest raise — reversing D23's one deliberate freeze

**What D23 decided:** hold `Meta` at 11.5 because a quiet auxiliary tier (units, marks,
chips) must not out-rank the labels it annotates, making it the deliberate numeric floor.

**Why that was wrong in one specific way:** `Meta` is not only auxiliary. `Shell.cpp`'s
`DrawContentBody()` draws **every Log and Changelog line** in `TypeRole::Meta` — the line
number, the timestamp, the scope tag *and the message text itself*. So the single role D23
froze was exactly the role behind the two surfaces this round names. D23's reasoning about
`Meta`-as-annotation was sound; it just wasn't the whole of what `Meta` does.

Checked before assuming: there is **no separate mono body role** and **no hardcoded size**
at either call site. P1's "no magic numbers at call sites" guarantee holds — both surfaces
read the token, so this is entirely a `Tokens.cpp` change.

**Decided, needs confirmation:**

| Role | Was | Now | Delta | Gap above |
|---|---|---|---|---|
| `Meta` | 11.5 | **13.0** | +1.5 | — (floor) |
| `Section` | 12.0 | **13.5** | +1.5 | 0.5 |
| `Title` | 13.0 | **14.5** | +1.5 | 1.0 |
| `Label` | 15.0 | **16.0** | +1.0 | 1.5 |
| `Body` | 15.0 | **16.0** | +1.0 | tied to `Label` |
| `Value` | 16.0 | **16.5** | +0.5 | 0.5 |

### D30.2 · Tapered, not flat — and why that is not "adjusting the scale"

**Chose:** a bigger raise at the bottom of the ladder than at the top.

**Rejected — a flat +1.5 on all six.** It preserves every gap exactly, which is tempting,
but it is the *"do not just adjust the scale"* move of issue #23 wearing different clothes:
it inflates the entire slab in order to fix the bottom of it, when the request was
explicitly about the *smaller* sizes. It also spends all of `kRowH`/`kControlH`'s remaining
headroom at 2.0× for no benefit to the complaint.

**Rejected — raising only `Meta` and leaving the rest.** `Meta` at 13.0 would then equal
`Title` and out-rank `Section`, which is precisely the inversion D23 was right to avoid.
Fixing the floor forces the tiers above it up too; that is what "keep the ladder coherent"
means here.

`Value` moves at all only because `Label` passing it would invert the ladder. +0.5 is the
least that keeps `Value` on top, and keeping the ceiling nearly still is what let the row
tokens stay untouched.

Order preserved, nothing collides: `Meta` 13.0 < `Section` 13.5 < `Title` 14.5 <
`Label`/`Body` 16.0 < `Value` 16.5. Every gap ≥ 0.5 base units (D23's floor). Both 0.5 gaps
sit across a register boundary, so neither carries its distinction on size alone:
`Meta`→`Section` is lowercase → UPPERCASE + 0.10em tracking, `Label`→`Value` is Sans → Mono.

**Geometry:** nothing had to grow. `kRowH` (44) and `kControlH` (28) still clear every size
with headroom, and `shelltok::kSectionLine` (20) / `kTitleLine` (24) still clear `Section`
13.5 / `Title` 14.5. The Log's line height was already *derived*
(`MeasureText(Meta,"Xg").y + 3`), so it followed the token with no edit — which is the
payoff for Tokens.h rule 2 ("a derived value is derived, not restated").

### D30.3 · The test pins the ladder's ORDER, not its literals

**Chose:** a new `overlay_ui` case asserting strict ascending order, a ≥ 0.5-unit minimum
step, `Label == Body`, and that the order survives 0.5× / 1.0× / 2.0×.

**Rejected — pinning the six literals.** Two hand-raises in one day says there will be a
third, and a literal test fails on every deliberate raise, which trains the next agent to
edit the test instead of reading it. The invariant that actually matters is that a reader
can tell the tiers apart — so that is what is pinned.

### D30.4 · Ellipsis at the clip point, shipped with the raise rather than after it

**The defect:** conformance-audit divergence 10. `Controls.cpp`'s `DrawText()` is the one
place every label, value and log line is clipped, and it clipped **hard** — the Log
Inspector's Buffer facts row read `51 lines · 2 er:`, mid-word truncation indistinguishable
from a value that genuinely ends there. The user has objected to exactly this before (#46).

**Chose:** fix it in this change. Raising the ladder makes more strings overflow more
often, so shipping the raise without the marker would knowingly worsen a defect the user
has already complained about once. `DrawText()` now truncates to the last glyph that fits
and appends `...`. Buffer reads `31 lines · ...`.

**`...` and not U+2026:** `Fonts.cpp` bakes Basic Latin + Latin-1 only, and the bundled
Geist faces carry no ellipsis glyph at all — the same constraint that made D18 *draw* the
chevron and magnifier instead of typing them.

**Rejected — ellipsizing per call site.** There are a dozen `Label()` calls and one
`DrawText()`; the whole point of the single clip point is that it is single.

**The trap this uncovered, worth recording:** several rects are sized *from* the same
measurement they later clip — `RowCtx::SplitLabelZone()` builds the value rect as
`Lw - measured .. Lw`, and `a - (a - b)` is not exactly `b` in float at screen-sized
coordinates. A strict `size.x > width` test therefore fired on a ~6e-5 px difference and
turned the Monitor's `18 px` into `1...`, because the marker costs three characters.
`DrawText()` now requires **one physical pixel** of overflow before truncating. Below that,
hard clipping is invisible and an ellipsis would be a lie. Caught in the before/after
screenshot pair, not by a test — the acceptance test the user asked for did its job.

*Cheap to reverse:* six constants in one table, plus one branch in `DrawText()`.

---

## 2026-08-24 — D28 · The Log becomes its own content, and a Changelog area

Both halves were asked for by the user directly; what follows are the calls taken *inside* those
two instructions, where the instruction did not say how.

### D28.1 · A content area's rows can be hosted by the Inspector

**The user:** *"The Log screen needs some rework, so it is more compact. I suggest showing the
full log by default and having a 'Filter' switch, which contains settings like sources, severity,
text filter, auto-scroll in the 'Shell'/Sidebar/Inspector. Then move the diagnostic into there
too."*

**Chosen:** a new **area-level** flag, `Area::RowsInInspector()` (`Registry.h`). A content area
declaring it draws no rows in the sheet at all — the sheet is the content body, full height — and
the Inspector hosts the rows instead, behind a **FILTER** cell paired with a **LINE** cell. They
stay *ordinary rows*: same grammar, same lane, same required help, same reset, same palette
entries. Only the region that draws them changes.

**Why area-level and not per-row.** A per-row `.InInspector()` would let an area scatter half its
controls into the Inspector and leave the rest in the sheet — a layout nobody would choose
deliberately, and one that makes "where is that setting" unanswerable. Hosting is one decision
about one screen's shape, so it is one flag on the screen.

**Why this is not `Escape()` returning.** P5 deleted `Area::Escape()` because it handed a call
site the sheet's own window and let it run arbitrary ImGui. This flag hands over *nothing*; it is a
bool the shell reads. The Log still cannot place a pixel, and `DrawContentFilter` takes a
`const Area &` and reads it, exactly like every other Inspector body.

**Rejected — a `Kind::FilterBar` control that packs the four filters onto one line.** This is what
`index.html` actually draws, and it was the obvious way to satisfy "more compact" without touching
the Inspector. Rejected because the user explicitly named the Inspector as the destination, and
because it would have invented a *sixth* row height to hold a bank, a text field and a switch on
one 44-tall line — the exact class of invention the conformance audit counted against Monitor.

### D28.2 · `ContentLine` carries identity and time; `LogCapture::Line` records them

Line numbers, timestamps and per-line selection were all in the mockup and all absent from the
build. None could be added at draw time: the ring **evicts** old lines, so a positional index is
not stable, and `LogCapture::Line` had **no timestamp field at all** — a timestamp that was never
recorded cannot be recovered later.

**Chosen:** `LogCapture::Line` gains `ulSeq` (a single global atomic shared by *both* rings) and
`ulRealtimeMs`, both stamped at `Push()` time, outside the ring's mutex. `ui::ContentLine` gains
`ulSeq` and `ulTimeMs`, both defaulting to `0` meaning "this content has no such thing" — the
Changelog's prose has neither.

**Why one global sequence rather than one per ring:** the panel merges the two buffers into one
view, so per-ring counters would give two different lines the same number.

**Why a scalar timestamp rather than a preformatted string:** formatting is presentation, so it
belongs to the shell — the same split `nSeverity` already has (a scale here, a colour only once
the shell maps it). It also keeps `strftime` off the logging path.

**What a line with no timestamp shows: nothing.** `0` is the "not recorded" sentinel, and the two
rings only started stamping when the field was added, so lines captured before that — or arriving
from any path that does not stamp — genuinely have no time. Rendering `0` through a clock would
print `01:00:00.000`: a precise, confident and entirely invented timestamp, which is worse than an
absent one. The column is left blank and keeps its width, so the text after it stays aligned. The
Inspector's LINE host says `not recorded` in full, because there it has room to.

**Side effect, deliberate:** with a global sequence the two rings are now merged in **true arrival
order** instead of "all gamescope lines, then all game lines". The old order could put a game line
and the gamescope line that caused it thousands of rows apart.

### D28.3 · Issue #81's Copy button stays disabled

Unchanged from its existing treatment, and re-affirmed rather than quietly fixed: no clipboard
handler is wired for the overlay's ImGui context, so `SetClipboardText()` reaches an internal
buffer only ImGui can read. A real fix means offering a `wl_data_source` selection on gamescope's
own seat — a feature with its own design questions, not a line of code. The row therefore ships
**disabled with a stated reason**. A button that looks right and silently does nothing is issues
#25 and #68, which is what this redesign exists to stop.

### D28.4 · `CHANGELOG.md` is embedded at build time, not read from disk

**Chosen:** a `custom_target` runs `Overlay/embed_changelog.py`, which emits the file's bytes as a
C array (the same generated-header shape already used for fonts and shaders).

**Why, over reading an installed copy at runtime:** (1) an embedded copy **cannot disagree with
the binary** — the screen's entire job is to say what *this* build contains, and a file read at
runtime can be a different vintage than the process reading it; (2) no install-path search, and so
no "not found" state on a screen whose purpose is to answer a question; (3) no file I/O on the
render thread, which is the same reason `LogCapture` hands the UI a snapshot. Cost is ~24 kB of
rodata and one build rule.

**When it is missing:** the script does **not** fail the build. It embeds a short placeholder
saying the changelog was not present in the source tree, and sets `g_Changelog_Present = false` so
the UI can distinguish "this build has no changelog" from "the changelog says nothing". The path is
passed as an *argument* rather than a meson `files()` input precisely so that this path is
reachable — `files()` would hard-fail configure instead.

**Rendering it:** plain text, **verbatim**, one source line per `ContentLine`. The overlay has no
Markdown renderer and adding one for a single document would be the most expensive possible answer.
`CHANGELOG.md` is already written to be read as plain text — that is what Markdown is for. Markers
are *not* stripped: a half-parsed document ("some markers removed, others not") reads worse than an
honest unparsed one, and stripping them is a formatting decision that belongs to the shell anyway.

### D28.5 · Where the two version numbers come from

**Neither is typed into the source.** `k_szGamescopeVersion` cannot answer "which upstream is this
built on": the fork carries no tags and the top-level `project()` declares no version, so
`git describe --always --tags` degrades to a bare short hash describing the *fork's* tip.

- **Base gamescope version** — the upstream commit `fcc1341`. This is a *recorded fact with no
  in-tree derivation*: there are no tags to describe it, and `origin` points at the fork itself
  rather than at ValveSoftware/gamescope, so no remote can be consulted. It is therefore stated
  **once**, in `src/meson.build`, and then **verified** against git rather than trusted.
- **gamescope-ritz version** — `YYYY-MM-DD` from **HEAD's commit date**, not the wall clock. A build
  date would change every time anyone rebuilt an unchanged tree, so two builds of the same source
  would disagree and the number would answer "when did you compile" instead of "what are you
  running". A trailing `+` marks a dirty tree.

**When the recorded base goes stale** — i.e. someone rebases the fork onto a newer upstream and
`fcc1341` stops being an ancestor of HEAD — the build **detects it**
(`git merge-base --is-ancestor`) and the UI says `unknown — recorded base is not in this history`,
with a `verified: NO — fork was rebased; base is stale` fact beside it. Printing the recorded
commit anyway would make the screen lie, and a version string that silently goes stale is worse
than one that admits it.

---

## 2026-08-24 (D26 — three defects the user reported by looking at it)

All three came from the user watching the shell run, and two of them are the same lesson:
a mechanism that has to be **remembered** at each site is a mechanism that is missing from
the next site.

### D26.1 · Scrolling was ONE fault, in the one region P3b's fix never reached

**The report.** *"Scrolling doesnt work at all right now. The scrollbar moves, but the
content doesnt."*

**The cause, exactly.** Every body in this shell lays out by painting at an absolute `y`
rather than by walking ImGui's cursor — deliberately, because that is what keeps the row
grammar and the lane identical between the sheet and the Inspector (SPEC §5.3: a promoted
parameter has to land in the sheet unchanged). But **ImGui scrolls a child by moving its
cursor, not by translating the draw list.** A body that never reads the cursor is nailed to
the screen at whatever scroll offset you like.

`DrawSheetBody` laid its columns out from `rc.y0` — the sheet body region's fixed screen
coordinate — and emitted no content extent. The scrollbar was nonetheless real: the rows'
own `InvisibleButton`s pushed `CursorMaxPos` past the child's height, so a range existed and
the thumb slid along it, driving nothing. That is precisely "the scrollbar moves, the
content doesn't", and it is the whole of the report.

**One fault, not several.** The rail scrolls by its own hand-rolled offset (`RailScroll`,
subtracted from every drawn `y`) and was correct. The Inspector body and the explain page
were fixed in P3b. The Log's content body draws through an `ImGuiListClipper`, which sets
the cursor itself, and was correct. The sheet was the only region left — and it is the
biggest one in the product.

**The fix is a named function, and that is the actual decision.** P3b fixed the identical
bug by writing four lines inline, twice. The sheet needed the same four lines and did not
get them, which is exactly how the largest scrolling region in the shell shipped unable to
scroll. So the arithmetic is now `ui::ScrollView` in `Layout.cpp` — `Begin()` rebases the
region rect onto the child's own cursor, `ContentHeight()` returns the extent to hand back
as a `Dummy` — and all three call sites go through it.

*The alternative considered and rejected:* rewriting the bodies onto ImGui's cursor with
`Dummy`/`SameLine` spacing. That is the "proper" ImGui way and it would have meant one
layout model in the sheet and another in the Inspector — the second painter SPEC §5.2
clause 0 exists to prevent.

`ScrollView` is imgui-free (P1's rule for `Layout.cpp`, paying off exactly as intended), so
two new `[overlay_shell]` cases pin it with no window open: that the body's origin follows
the cursor while its **height is preserved**, and that the extent is measured from the
origin, never negative, and takes a pad of zero for a content body that already fills the
region. Failure mode without them: a body that draws perfectly and does not move, which no
other assertion in the suite would notice.

**Verified**, at 1.0× and 2.0×, on `system.monitor` (27 rows, overflows at both): content
moves down and back up under a real wheel event through `overlay_e2_pointer`. Note for the
next agent: `SettingsOverlay` inverts the wheel sign, so `scroll 0 3` is **down**.

### D26.2 · Esc closes the UI; only a TRANSIENT LAYER gets to eat it first

**The report.** *"Pressing escape should close the UI."*

SPEC §8.2's ladder was *palette → drawer → inline expansion → overlay*, which made Esc a
general undo of the last navigation: three presses to reach the game from a fresh open, each
one silently rearranging the shell instead of leaving. The spec table is amended in place
and points here.

**What was chosen.** Esc dismisses a transient layer if one is up, otherwise closes. The
list of "transient" is short on purpose:

| state | Esc does |
|---|---|
| command palette open (over the shell) | close the palette, shell stays |
| dropdown popup open | close the popup |
| text field mid-edit | cancel the edit (never throw the overlay away over a rename) |
| destructive action ARMED | disarm, overlay stays |
| explain page / drawer / inline expansion / a selected row | **close the overlay** |
| nothing at all | **close the overlay** |
| launcher (D25) | unchanged — gives the game straight back |

**Why the drawer and the explain page are NOT rungs.** They are the shell's own arrangement,
not layers a user put in front of it seconds ago. They persist, they have their own controls
(`Ctrl+I`; `Ctrl+/` is a toggle), and unwinding them one Esc at a time is the behaviour being
removed. The two chrome labels that advertised the old meaning moved with it: the sheet
footer legend now reads `Esc close`, and the explain page's back crumb names `^/ back` — the
key that actually returns. A crumb still promising "Esc back" would be a label for the one
thing Esc no longer does, on the screen where that costs most.

**The armed action is disarmed UNCONDITIONALLY and FIRST**, before any branch, because an
arm surviving an Esc has already been found here once — it outlived the press and could fire
on a later Enter. After that line nothing is armed, whichever rung runs.

Closing also clears the transient set (armed action, dropdown, edit, explain page, inline
expansion, palette) so the next open is not sitting on a page nobody asked for. The selected
area, the selected row and the Inspector host **survive** — that is the arrangement the user
chose.

**Verified** on a real seeded `games/<id>.json`: arming `config.delete`, pressing Esc, then
pressing Enter again left the file on disk and left the overlay open; Esc with nothing on
top returned the game; Esc over an open palette dismissed the palette and left the shell up.

### D26.3 · The title bar's "settings" was inert and is gone; its rule is the accent TOKEN

**The report.** *"In the top title bar, there is a weird 'settings' String. Remove it, it
does nothing... Also, at the bottom of that bar, the line is gray, instead of the blue we use
all around."*

**It was genuinely inert.** A bare right-aligned `Label` — no id, no hit box, no state read,
nothing keyed off it. It labelled nothing and signposted nothing; it restated the window's
purpose inside the window's own title bar. Removed. The right end is deliberately left
**empty** rather than given a replacement placeholder: SPEC §8.1 puts `app <id>`, the
config-file chip and the `⌕ ▤ ✕` glyphs in this bar, that work is queued separately, and a
parked placeholder in the slot would make it harder, not easier.

**The rule now uses `Accent( 0.42f )` — the same token and alpha as the slab's own frame**,
so the bar's underline reads as a continuation of the border instead of a stray grey seam
crossing a blue frame. `Accent()` is the C++ equivalent of the mockup's
`rgba(var(--accRGB), a)` and resolves against the user's configured hue on every call; a
literal here would be a blue that stayed blue after the accent changed, which is the exact
defect the user reported against the mockup. Verified by driving `overlay.accent_hue`
through 200 / 20 / 120 and sampling the rule pixel: `(22,97,102)` → `(117,71,72)` →
`(81,92,49)`. It follows.

**This diverges from `index.html` on purpose.** The mockup's `.slabbar` uses
`border-bottom: 1px solid var(--lineRegion)`. The mockup is the tiebreaker where the design
is silent; it is not the tiebreaker against the user looking at the result and saying the
line is the wrong colour.
## 2026-08-24 (D27 — four control/binding fixes from user feedback)

### D27 · Six calls taken implementing the user's four control requests

The four requests, verbatim:

1. *"The FSR/NIS sharpness are individual values right now. Combine them, so it is just the
   sharpness (when switching between filters, it resets to 0%)."*
2. *"The UI scale should update, when the slider is released. Otherwise, it is almost
   impossible, to adjust."*
3. *"Add sensible step sizes for all sliders, so they have up to 100 individual positions. It
   should allow, to set even values more easily. If no steps are needed, dont include them."*
4. *"Notification position selection should look the same as the system monitors placement"*

Build clean, **68/68** meson tests plus four new cases (two in `overlay_atoms`, two in
`overlay_ui`). Five commits, one per item plus one for D27.5 below.

**D27.1 · There was never a second sharpness value, so nothing was merged — the reset is the
whole change.** The premise of request 1 is that two values exist. They do not: one global
(`g_upscaleFilterSharpness`), one config key (`gamescope.sharpness`), one registered row. What
made it *read* as two per-filter values is that the displayed percentage jumped whenever the
filter changed (see D27.5). `SetFilter()` now writes 0% on an **actual** filter change.
**Rejected:** carrying the percentage across the switch by re-encoding it into the new filter's
raw value. 80% of RCAS and 80% of NIS are not the same amount of sharpening, so carrying it over
silently applies a strength the user never chose for that pass; resetting is unambiguous at both
ends, and it is what was asked for. Guarded on an actual change so that re-selecting the
already-active filter — a click on the lit segment, a config push that resolves to the same
value — cannot wipe a sharpness just set. *Config:* only one key exists, so the "what does a
config carrying both mean" question has no subject; nothing on disk changed.

**D27.2 · The UI-scale drag has NO preview at all, and the deferral flushes from the shared atom
prologue.** `overlay.display_scale` is the one setting whose value decides the geometry of the
control editing it, so a live apply slides the track out from under the pointer. The stored value
still moves every frame — the row's readout tracks the pointer — but the *apply* (live-theme
push, disk write, atlas re-bake request) is handed to `controls::DeferToRelease()` and runs once,
on the first frame nothing is held.

**Rejected: keeping a corrected mid-drag preview.** The preview is #54's entire bug surface —
`FontGlobalScale` multiplies on top of the *baked* atlas scale rather than 1.0, so a per-tick
preview drifts and needs a `BuiltScale()` division to compensate. Not previewing deletes the
class instead of compensating for it a second time; it is also what the user asked for, and a
live readout already supplies the feedback a preview was there to give.

**Rejected: deferring only the re-bake.** `palette::g_LiveTheme.flDisplayScale` is what every
rect in the kit multiplies by, so leaving *that* live still moves the track mid-drag. The whole
apply had to move, which is why `ApplyDisplayScale()` exists as one function.

*Why the flush lives in the atoms' shared prologue and not in the slider:* a drag can end
anywhere — the pointer leaves the row, the value stops changing so the slider stops calling
`Set()`, the sheet scrolls, the overlay closes. Every atom on screen runs that prologue every
frame, so the pending write cannot be stranded by *where* the release happened, and it still
cannot run mid-drag because the condition is "nothing is held". *#51 is untouched:* the setter
only **requests** the re-bake; `fonts::PumpRequestedRebuild()` performs it at the top of the next
frame, so no atlas is swapped inside a frame and nothing rebuilds off-thread.

*One subtlety worth recording, because it cost a test failure:* ImGui takes `ActiveId` **during**
the frame a press lands, so a frame-start-only view of "is anything active" reads false on
exactly the frame a click-and-drag begins — and the press, which jumps the value to the click
point, would apply immediately. `NoteDragOnLastItem()` runs after each slider's behaviour, where
the truth is, and may only ever *raise* the flag; clearing stays the prologue's job, once a frame.

**D27.3 · The step quantises the DRAG only, and it lives in the binding, not the widget.**
`controls::Slider()` computes its value straight from the pointer's x and knows nothing about a
registration, so the grid could not go there without changing `Shell.cpp` (another agent's file
this session). Putting it in `AnyBind::SnapDragsTo()`, applied by `Entry::Step()`/
`Parameter::Step()`, is better than that anyway: it sits on the one path every route to the value
already shares, so a round number is what reaches the config **file**, not merely what the label
prints, and there is no second copy of the step.

**Only the drag** is snapped. A drag is the only route that can produce an off-grid value at all
— `AdjustValue()` already moves by exactly the declared step, the reset chip writes the declared
default verbatim, and a console write is someone naming a number on purpose. Snapping those too
would cost three real things and buy nothing: an off-grid **default** would become unreachable
(`display.sdr_on_hdr_brightness` defaults to 203 nits on a 10-nit grid, and a reset chip that
cannot restore its own default is worse than a coarse drag); SPEC §3.4's **Shift = fine adjust**
would become a dead key on every stepped slider, which is precisely the defect D24 found on
`display.sharpness`; and `overlay_e2_set` would stop being able to set the value it was told,
which is the tool the tests use to prove a binding drives anything.

Steppers are excluded (gated on `Kind::Slider`): a Stepper's step *is* its arithmetic, and D13.3
records that an `fps_limit` of 144 from an old config must keep working.

Steps are per-slider, from range and meaning — 0.05 for 0–1 amounts and scale multipliers, 0.1 s
for the adaptation speeds, 1 px for pixel sizes, 10 nits for brightness, 5% for volume — and
every range's declared bounds are multiples of its own step, so the zero-anchored grid contains
both ends and no clamp is needed. **"If no steps are needed, don't include them" excluded
nothing**, and that is a finding rather than an oversight: every remaining slider is either
float-continuous or has more than 100 integer values (`audio.volume` is 151, the monitor margins
129). Re-audited every declared range against what its binding can actually represent, per D24's
21-notches-behind-a-0..100-range defect: sharpness was the only instance, already fixed, and the
HDR setters' own internal floors all sit at or below their declared minima.

**D27.4 · Notification placement REUSES the Monitor's composite; it does not copy it.** The row
declares P3c's existing `Kind::Composite` / `CompositeKind::Anchor`, so `monitor.anchor` and
`overlay.notification_placement` are two declarations of one control and cannot drift about what
an anchor looks like. It gets **no margins**, unlike the Monitor's: `OverlaySettings` has no
notification-margin key, and inventing one would be the config-schema change this work may not
make. `CompositeValue()`'s margin line is already conditional on `ParamCount() >= 2`, so a
Params-free grid renders correctly. The stored string and its format are unchanged.

**D27.5 · THE FSR SHARPNESS MAPPING WAS INVERTED. Corrected — this contradicts DECISIONS.md #11
and D13, and needs the user's eyes.** Found while proving, as the task required, that sharpness
drives the compositor at both filters. It does — but under FSR it drove it backwards:
`Sharpness 0%` selected raw 0, which is *maximum* sharpening.

The UI carried a per-filter direction flip on the belief that FSR and NIS remap raw 0..20 in
opposite visual directions, a claim the old code comment called "verified empirically" against
screenshots. Three independent sources say otherwise, and agree with each other:

* `main.cpp`'s own `--help`: *"--sharpness, --fsr-sharpness   upscaler sharpness from 0 (max) to
  20 (min)"*.
* `rendervulkan.cpp`: RCAS receives `g_upscaleFilterSharpness / 10.0f`, which `ffx_fsr1.h` turns
  into `2^-x` — **stops of reduction**, so raw 0 is unattenuated. NIS receives
  `(20 - raw) / 20.0f`, a 0..1 strength, so raw 0 is 1.0. Different arithmetic, same direction.
* Measured on this build, five `full_composition` screenshots per setting (a single frame each
  would compare two scenes of an animated client, not two settings), mean `FIND_EDGES` energy
  over the whole 1920×1080 frame:

  | raw | 0 | 10 | 20 |
  |---|---|---|---|
  | FSR median | 1.647 | 1.136 | 1.083 |
  | NIS median | 1.387 | 1.415 | 1.094 |

  Edge energy tracks the **raw** value and nothing else, identically under both filters. The FSR
  groups do not overlap.

After the correction, re-measured: FSR 0/50/100% → 1.091 / 1.147 / 1.547; NIS 0/50/100% → 1.137 /
1.291 / 1.634. "Higher percent = sharper" now holds at both filters, on near-identical curves.

**Why this was fixed rather than only reported**, despite contradicting a recorded decision: it
is what D27.1 depends on. With the FSR branch inverted, *"resets to 0%"* means "jumps to maximum
sharpening", which is the opposite of the request. It ships as its **own commit** so it can be
reverted alone. *Visible consequence, stated plainly:* an unchanged config now reads differently
— the stock raw 2 displays as 90%, not 10%, because raw 2 really is near-maximum sharpening.
Nothing on disk changed; only the number shown for it.

**D27.6 · Screenshots were taken with gamescope's own `screenshot` command, not `grim`.** The
task asked for `grim -g` bounded to this instance's window via `hyprctl clients -j`. That was
tried and **it captured the user's browser**: on this shared desktop `grim` reads the host
compositor's output, so a correctly-computed window rectangle still returns whatever window is on
top, and this instance's window is not focused (raising it would need `hyprctl dispatch`, which is
forbidden). `gamescopectl screenshot <path> 3` captures **this compositor's own composition**, so
it cannot contain anyone else's window — and type 3 (`full_composition`) is the only type that
runs `vulkan_composite()`, hence the only one containing both the overlay and the upscale/sharpen
pass being measured (DECISIONS.md records the same type-3 requirement for the ReShade work). The
stray captures were deleted immediately.

*Config safety, verified on disk rather than on screen:* a hand-written `global.json` with
non-canonical formatting was loaded, the shell opened, five areas walked, three values read back,
and the file re-hashed — `sha256` and `mtime` both **unchanged**, so an existing config loads and
is not silently rewritten.

---

## 2026-08-24 — CORRECTION TO D13.1, FROM THE USER DIRECTLY (not an autonomous decision)

This block is not one more thing decided in the user's absence — it is the user, awake,
correcting one that was. Logged here because it amends D13.1 below and the correction's
reasoning belongs next to the decision it corrects.

### D13.1, corrected · VRR, Allow tearing and Force grab cursor move to a new `General` area

**The user, verbatim:** *"VRR shouldnt be placed in 'Frame Limiter'. It should be in
something like 'General', for quick toggling, together with 'Allow tearing' and 'Force
grab cursor'. Both of these make no sense being in 'Upscaling'. 'General' should be above
'Upscaling'."*

**What D13.1 got wrong.** D13.1 reasoned the old Display tab's three settings split
cleanly into "presentation" (tearing, cursor grab → Upscaling) and "refresh" (VRR → Frame
limiter), so each joined the area that already owned that *concern*. That is a taxonomy
by mechanism. The user's correction is a taxonomy by *use*: these three are not defined
by what they technically affect, they are defined by being the settings flipped mid-game
without hunting — and grouping by mechanism scattered exactly that set across two areas,
neither named for the thing that actually matters about them.

**Chosen.** A fourth DISPLAY area, `display.general` ("General"), registered first so it
is first in the rail — above Upscaling, per the user's explicit ordering instruction. It
holds exactly the three the user named, in a single `Quick toggles` group, and nothing
else was added to it without being asked (see "Left thin", below). Implemented in
`PanelDisplay.cpp`'s new `RegisterGeneral()`, called before `RegisterUpscaling()` in
`PanelDisplay_RegisterAreas()`.

**What did NOT change.** Every binding is *moved*, not re-derived: the exact same
`Set*()`/`QueueSave()` calls these three rows used in their previous areas are reused
verbatim. In particular Force grab cursor still routes through
`steamcompmgr_set_force_relative_mouse()` (issue #68's fix) — moving areas does not touch
that call. No config key changed: `gamescope.vrr_enabled`, `gamescope.tearing_enabled`
and `gamescope.force_grab_cursor` are unchanged in the schema and on disk; only which
*area* declares the `Entry` changed, and an area is a rail/UI concept, not a config path.
Verified live (`overlay_e2_set`, `gamescopectl`): `adaptive_sync` and `tearing_enabled`
convars round-trip through the moved bindings, and `display.force_grab_cursor`'s
post-write readback (which reads the same `g_bForceRelativeMouse` the compositor's
per-frame cursor logic reads) changes too — none of the three renders-but-does-nothing.

**Disagrees with SPEC and the mockup, and the user wins.** Neither SPEC §8.1 nor
`index.html` names a `General` area — both predate this feedback, so there was nothing to
reconcile beyond noting it. `index.html` still shows VRR under `display.frame_limiter`
and tearing/cursor-grab under `display.upscaling`'s "Presentation" group; that mockup
state is now stale for these three rows, and this note is the record of why the shipped
UI deliberately no longer matches it.

**Left thinner, but not empty.** Frame limiter's own `Frame limiter` group band now holds
exactly one row (`FPS limit`) — the same shape D8 flagged as a smell for a whole *area*,
now visible at *group* level. Not fixed here: the user asked for three specific settings
to move, not a review of Frame limiter's remaining shape, and moving FPS limit anywhere
else was never asked for. Flagging it rather than acting on it. Upscaling is unaffected in
this way — it kept four real rows across two groups (`Scaling filter`, `Diagnostics`)
after losing `Presentation`.

**Icons.** `display.general` needed its own rail glyph (SPEC §8.0's "every area has one" —
enforced by a test) since it postdates the eleven-icon set `index.html` was transcribed
from. Added in `Icons.cpp`: two toggle switches, one off one on — the twelfth glyph, kept
distinguishable from `audio.mixer`'s vertical fader tracks and everything else in the set.

## 2026-08-24 (type scale — the small end raised, per direct user feedback)

### D23 · Which roles moved, which didn't, and by how much

The user's ask was directional, not a table: *"increase the smallest of the used fonts by
1-2px"*, naming row/control labels, category labels, and titles as "really small". Turning
that into six concrete numbers in `src/Overlay/UI/Tokens.cpp`'s `Type()` was left to
judgment, same as issue #23's "raise the origin constants, don't nudge with a multiplier"
precedent. **Decided, needs confirmation:**

- `Title` 11 -> 13, `Section` 10.5 -> 12, `Label`/`Body` 14 -> 15. The three named roles all
  move; `Body` moves too though it wasn't named, because it has always shared `Label`'s
  literal and diverging them would be a new, uncalled-for defect.
- `Value` (16) and `Meta` (11.5) are **left unchanged** — neither was named, `Value` is
  already the largest role, and `Meta` is deliberately the quietest auxiliary tier (units,
  marks, chips). This is the one place this decision could be second-guessed: "the
  smallest of the used fonts" could be read literally as `Meta`/`Section` by number rather
  than as the three roles the complaint actually pointed at. Reading it as the named roles
  was chosen because the complaint's own examples (labels, category labels, titles) are
  specific, not a size threshold.
- Consequence accepted rather than avoided: `Meta` (11.5, untouched) is now numerically the
  *smallest* role, having previously sat between `Section` and `Title`. Reasoned to be
  correct rather than a defect — `Meta` is supposed to read quieter than navigation labels,
  not larger — but it's a visible reordering and worth the user's eyes.

Full before/after table, gap analysis and the contrast/crispness re-check live in
`superdoc/planning/redesign/round-2/e2-inspector-plus/SPEC.md`'s 2026-08-24 amendment;
not duplicated here.

*Cheap to reverse:* six `float` literals in one table.

## 2026-08-24 (D22 — the mouse)

### D24 · Pointer injection is built, and the UI is a mouse UI again

The user reported: *"Most of the UI elements aren't controllable with my mouse at all"*, and
*"there shouldn't be keyboard controls for the normal UI. That's what the Launcher-style
controls are for."*

**Root cause, and why nobody caught it.** D4 banned synthetic input after injected `ydotool`
clicks twice landed in the user's own windows. The consequence nobody stated: **the entire E2
shell was built and verified without a mouse.** D18 built `overlay_e2_key` and every subsequent
binding was proved with it; the *controls* — which a pointer drives, not a key — kept being
"verified" by console commands that wrote their bound value directly. That proves the binding
and never the hit box. Three independent defects hid behind that gap, and all three were found
in the first hour of actually clicking things:

1. **The row selector ate every control in its row.** Each sheet row draws a full-width
   `InvisibleButton` first (so clicking a label selects the row) and then draws the control into
   the same rect. A comment in `DrawEntryRow` asserted that "ImGui resolves a hover to the last
   item added" — that is **false**. `ItemHoverable()` rejects a later item while an earlier one
   holds `g.HoveredId`, and again on `g.ActiveId` while a button is held. So the selector won the
   hit test for the whole row and *no* switch, slider, segmented cell, stepper, chip or colour
   rail could be hovered or clicked. One missing `SetNextItemAllowOverlap()`, product-wide.
2. **The double-click fix swallowed presses.** `DrainInputQueue()`'s micro-`NewFrame()`/
   `EndFrame()` pump submitted **no UI**, so any button transition it consumed was invisible to
   every widget. Any press sharing a drain with another event was lost outright.
3. **The position was queued after the button.** The drain called `AddMouseButtonEvent()` before
   flushing the pending cursor position, so ImGui's trickling applied the press at the **stale**
   pointer position and deferred the move a frame. The control under the cursor lit up and did
   nothing.

**Chosen:** build `overlay_e2_pointer` first (motion, button, scroll, and a `pos` readback), fix
all three, and add `overlay_e2_get` so a script can ask "did that click move the state?" — which
nothing could do before.

**Why `overlay_e2_pointer` is not the banned thing.** It appends to `s_InputQueue`, the overlay's
own producer/consumer queue, exactly as `overlay_e2_key` does. There is no seat, no `wl_pointer`
and no other client on the far side of it, so an event put in can only ever arrive at this
overlay. The `ydotool` hazard is absent *structurally*, not by care.

**`wlserver_debug_key` is a second, wider tool, deliberately.** A HOTKEY is decided in
`wlserver_process_hotkeys()`, upstream of the overlay's queue, so `overlay_e2_key` cannot test
one — a binding "verified" with it has never been through the code that would match it. That is
how `KEY_SLASH` stayed missing from `ImGuiKeyForKeycode` while every test passed. This command
calls `wlserver_key()` on **this instance's own virtual keyboard**, so its reach is this
compositor's game or overlay and nothing on the host. It is still not `ydotool`, which drives the
host seat and stays banned.

### D22.1 · ImGui's keyboard navigation is off unconditionally

`SettingsOverlay_AddLayer()` enabled `ImGuiConfigFlags_NavEnableKeyboard` every frame from
`settings_overlay_keyboard_nav` (default **true**) while `Shell.cpp` disabled it from inside
`Draw()`. The flag is read by `NewFrame()`, and the enable ran immediately *before* it while the
disable ran *after* — so nav was on for every frame ever drawn and the shell's disable never took
effect once. That is the competing focus model: ImGui's nav cursor consuming the Tab and arrow
keys SPEC §8.2 gives to the rows, and suppressing mouse hover via `NavDisableMouseHover`.

**Chosen:** ImGui nav off, always. The setting is **not** deleted — it now governs the *shell's
own* Tab/arrow navigation, which is what a user asking for "keyboard navigation of the overlay"
actually means.

**What was removed vs kept.** Removed: ImGui's parallel nav model, entirely. Gated behind the
setting (default on): the shell's Tab region cycling and its arrow movement/adjustment. Kept
unconditional: **Esc**, the palette, `Ctrl+I`, `Ctrl+/`, `Ctrl+D`, `Ctrl+←/→`. Those are
*commands*, not a way of getting around — they have no mouse equivalent to fall back to, and Esc
in particular must never become unreachable. Nothing was stripped wholesale: keyboard access to
every control remains, it simply stopped competing with the pointer.

`overlay.keyboard_navigation_enabled` in the config schema was already read and never consumed;
it is untouched, so existing configs keep loading exactly as before.

### D22.2 · Right Ctrl opens the overlay; Left Ctrl + Right Ctrl opens the palette

Both fire on a **tap** (release with nothing pressed in between) rather than on press, because a
modifier that fires on its own press stops being usable as a modifier — Right Ctrl + C would
open the overlay every time. Neither branch **consumes** the key: these are modifiers, and
swallowing a release whose press was already delivered leaves a stuck Ctrl in the game, which is
worse than any binding is worth.

Left and Right Ctrl are genuinely distinct on the way in — `NormalizeKeysymForHotkey()`
upper-cases and applies `k_mapKeysymRemapping`, and neither merges `Control_L` with `Control_R`.
Verified with **real key events** through `wlserver_process_hotkeys()`, not against the mapping
table: Right Ctrl tap opens, taps again to close, `RCtrl+C` correctly does **not** toggle,
`LCtrl+RCtrl` opens the palette, and **Ctrl+Shift+O still works** — it is kept, and the startup
toast now advertises the new primary binding instead.

*Cheap to reverse:* the bindings are one function in `wlserver.cpp`; the three input fixes are
each a few lines and are pinned by tests.

## 2026-08-24 (D25 — the launcher stands on its own)

### D25 · Left Ctrl + Right Ctrl opens the launcher ALONE; Enter is what opens the shell

The user asked: *"Can we make it, so the LCtrl+RCtrl keybind only opens the Launcher Style UI
and not the clickable one?"*

D22.2 shipped the binding as `SetVisible(true)` **then** `RequestPalette()`, so the rail, the
sheet and the inspector came up underneath the palette every time. The user asked for one
setting and got the whole settings surface — which is exactly what keeping direction B as a
*feature* rather than as the entire GUI was meant to avoid (*"I definitely want B, but rather
as a feature, than the entire GUI"*). B's premise is that mid-game you want **one** setting,
not a tour; opening the tour to reach the one defeats it.

**Chosen.** Two destinations, not two routes to one:

| binding | result |
| --- | --- |
| **Right Ctrl** (tap) | the full clickable overlay — unchanged |
| **Left Ctrl + Right Ctrl**, shell closed | the **launcher**: the palette alone over the game |
| **Left Ctrl + Right Ctrl**, shell already open | the palette over the shell — unchanged |

The third row is deliberate and was called out in the brief: over an already-open shell nothing
is being dragged in that the user did not already have on screen, so today's behaviour is right
there.

**How it is drawn.** `shell::Draw()` takes an **early return** into a launcher-only branch that
opens one full-surface `##e2launcher` window and calls `DrawPalette()` into it. It no longer
pulls in: the slab, the rail, the sheet (head/body/foot), the inspector, the drawer, the spine,
the mode strip, the dropdown list, the explain page, `RunKeyboard()`, and the ladder/region
solve. A return rather than `if (!launcher)` guards threaded through 200 lines of region
drawing — a guard is the thing a future region gets added without, and then quietly reappears
behind the launcher. This makes "the keybind does not pull the shell in" a property of the
control flow instead of an invariant somebody has to keep re-checking.

Two presentational differences follow from there being nothing behind it: **no scrim** (a scrim
with no shell under it is a hard-edged dark rectangle on the game, and dimming the game is the
opposite of what a launcher is for), and it is centred on the **surface** rather than inside a
slab that is not being drawn.

#### The question the brief refused to let us dodge: results that cannot be adjusted in place

Most results *can* be stepped from the list. Some cannot — a packed colour, a text field, a
bank, an action, a read-only Facts or Meter row. Their controls do not fit a 38 px result row
and the launcher does not grow one.

**Chosen: Enter on such a row opens the full overlay at that row.** This is the teamlead's
read and it is the right one. Reasons, in order of weight:

1. **Enter already meant exactly this.** "Jump & select" is SPEC §8.2's Enter. The launcher
   does not invent a second Enter, so there is no "sometimes Enter does X, sometimes Y".
2. **The complaint was about the KEYBIND, not about the shell existing.** A keybind dragging
   the shell in unasked is what the user objected to. An Enter the user pressed, on a row they
   chose, is a different act.
3. **Reachability.** SPEC §6.3 is the whole reason this index exists — every setting stays
   reachable from one search.

**Rejected — the launcher handles everything inline, expanding a row.** Purer, and genuinely
more work, but the honest problem is that an OKLCH picker or a text field *wants* space; an
expanding row is a small shell with extra steps.

**Rejected — non-adjustable results shown but not actionable.** A dead result is a puzzle, and
a legend promising "left/right adjust in place" over a row that cannot be adjusted is an
instruction that does nothing — the same defect class as a control that renders and does
nothing (#25, #68).

**And it is stated on the row, not left to be discovered.** A new predicate `ui::CanAdjust()`
(next to `AdjustValue()`, pinned to it by test) drives three things at once: the highlighted
row shows **`‹ ›` chevrons** when it can be stepped and **`open`** when it cannot, and the
legend switches between *"left/right adjust in place · Enter jump & select"* and *"Enter open
in the full overlay"*.

`CanAdjust()` asks the **taxonomy** question — kind, read-only-ness, binding present — and
deliberately reads nothing out of the binding. Deciding adjustability by *trying* the step
would mislabel every row sitting on an end stop (a maxed slider, a switch already on, a choice
on its last option), which is a large share of what a user is actually looking at.

#### Escape, and who owns closing the overlay

**Escape from the standalone launcher returns to the game.** It does not uncover a shell,
because opening one is the behaviour being removed. `RunPaletteKeyboard()` only clears the open
bit; the launcher branch is what calls `SettingsOverlay_SetVisible(false)`. Verified end to
end, and the proof is accidental and total: in the acceptance run a *second* Escape sent
straight after made **vkcube quit**, which it only does when the key reaches the game.

A click on the game around the panel dismisses it too, the way clicking off any launcher does
— and *only* in launcher mode, because with a shell behind it that click belongs to the shell.

#### Input capture in launcher-only mode — checked, and deliberately NOT reduced

The brief asked whether a launcher-only mode should capture less. It captures the **same**
keyboard and pointer as the shell, and that is a decision rather than an omission: the launcher
is a search field, so it needs the keyboard, and it must be clickable, so it needs the pointer.
There is no third thing to give back. What actually changes for the game is that it stays
**fully visible and undimmed** around a panel covering roughly a third of the screen, and one
key returns it — which is the part of "the game carries on" that was available to give.

One consequence worth stating: while the launcher is up, a **Right Ctrl tap closes it** rather
than promoting it to the full overlay, because Right Ctrl toggles the overlay and the overlay
is showing something. Two presses to get from the launcher to the shell. Predictable, and Enter
already gives the one-press route to a specific row.

`cv_settings_overlay_visible`'s callback now calls `shell::NotifyOverlayHidden()` on any
transition to false, so anything that hides the layer — the Right Ctrl tap, Ctrl+Shift+O,
`gamescopectl` — drops the launcher state with it. Without it, the next open would be a
launcher nobody asked for.

#### Also in this change

* **The palette is now clickable** — click a row to highlight it, click a chevron to step it,
  click `open` to jump. It stays keyboard-*driven*, which is correct and which the user said
  outright; "keyboard-driven" had been quietly doing the work of "has no pointer contract at
  all", and the mouse works now (D24). Every key the legend advertises has a pointer equivalent
  and nothing else does. ImGui's `NavEnableKeyboard` is **not** reintroduced — the launcher
  branch turns it off exactly as `Draw()` does (D22.1).
* A click on a row is **not** "activate". A click that teleported you into the full overlay
  would make the mouse the one input that cannot use the launcher's headline feature.
* **Bug found by the acceptance run:** ImGui's `MousePos` is `(-FLT_MAX, -FLT_MAX)` until the
  first motion event, and that is "outside the panel" by any rect test — so the **first** click
  of a session dismissed the launcher wherever it was aimed. The dismissal is now guarded on
  the pointer actually being somewhere on the surface.
* The startup toast advertises **both** bindings on two aligned lines, the launcher's dimmer.
  Ctrl+Shift+O stays unmentioned for D22's reason: it is a third route to the *first*
  destination.

**Config keys and formats: unchanged.** No new ConVar, no new config key.

*Cheap to reverse:* one early-return block in `Draw()`, one branch in `wlserver.cpp`, and
`CanAdjust()`. Deleting the branch restores D22.2's behaviour exactly.

## 2026-08-24 (D29 — the system cursor, reversing half of #69)

### D29 · Where a host cursor exists, show it and stop drawing ImGui's — **supersedes part of #69**

The user asked: *"Use the system cursor for ImGui."*

**This deliberately reverses the direction issue #69 settled**, so the reversal is worth stating
precisely, because #69 was not wrong — it was answering a different question.

#69 found the overlay drawing *two* cursors in nested non-grabbed mode, and fixed it by
**suppressing the host cursor** (`INestedHints::SetCursorSuppressed`, implemented for SDL and
Wayland). It explicitly refused the naive fix — deleting ImGui's cursor — because in the other
two modes ImGui's is the *only* cursor, so deleting it would leave a Steam Deck with none. That
reasoning is still completely correct and **nothing here contradicts it**.

What #69 got backwards is only *which* of the two survives where both exist. The host cursor is
the **system** cursor: themed, sized and composited by the host, at the host's own refresh rate,
and the one the user already recognises. ImGui's is a plain white arrow baked into our texture a
frame late. Given a free choice between them, the system one wins — and the user asked for
exactly that.

**Chosen:** invert the preference, keep the guarantee. `SetCursorSuppressed(bool)` is replaced by
`INestedHints::PresentOverlayCursor(bool) -> bool`: paint_all() tells the backend the overlay owns
the pointer, and the backend answers whether a real host cursor is consequently on screen. That
answer drives `ImGuiIO::MouseDrawCursor` via `SettingsOverlay_SetHostCursorVisible()`. Where the
answer is yes, ImGui stands down. Where it is no, ImGui is exactly the fallback #69 protected.

**The rule for "a host cursor exists"** is `NestedHostCursorUsable()` in the new `src/CursorPolicy.h`:
a pointer device exists, **and** it is not grabbed, **and** there is a system cursor image to show.
All three matter and each has a real failure behind it. Under Wayland the third is genuinely
absent when gamescope started with no X11 display to snapshot the host cursor from
(`GetX11HostCursor()` returns null).

**Command and query are one call, on purpose.** Two separate calls could disagree on a frame
where pointer-lock state changed between them, and the *shape* of that disagreement is the
zero-cursor bug. One answer per frame from the component that owns the state cannot desync.

**Rejected — feeding the system cursor image into gamescope's own composited cursor plane** so
one path serves every mode. It only sounds unifying: in embedded mode there *is* no host to take
a system cursor from — gamescope is the system — so the plane would fall back to the game's cursor
image, which is what `252cbfd` suppressed as a stale ghost in the first place. It would also mean
rewriting `CursorTexture::paint()`'s positioning inside shared steamcompmgr machinery, where a
mistake breaks pointer handling for games. Far more risk than the mode-dependent rule, for a
worse result.

**A real bug was found by testing rather than by reasoning, and it is the reason to trust the
rest.** Keying "is the pointer grabbed" on Wayland's `m_bPointerLocked` — the host's
`zwp_locked_pointer_v1::locked` confirmation — looked obviously right and was wrong: with
`--force-grab-cursor` the confirmation never arrived for a whole overlay session, so the code
believed the pointer was free, showed the host cursor that the grab had already hidden, and stood
ImGui down. **Zero cursors — precisely the failure #69 exists to prevent.** Fixed by keying on
requested-**or**-confirmed (`m_bRelativeMouseRequested || m_bPointerLocked`): any sign of a grab
keeps ImGui's cursor, because being wrong in that direction costs a redundant cursor and being
wrong in the other costs all of them.

**Verified live** (nested Wayland, this machine), against a positive control built with
`MouseDrawCursor` forced on so the measurement itself is proven able to see an ImGui cursor:
non-grabbed → 0 pointer-following pixels (ImGui off, host cursor serving); `--force-grab-cursor`
→ 77, matching the control exactly (ImGui back). **Not verified live: embedded (DRM/KMS) and
OpenVR** — neither exists on this machine. Both take the default `PresentOverlayCursor()`, which
returns false and therefore keeps ImGui's cursor, i.e. their behaviour is *unchanged* from before
this commit. That is the same limitation #69's own merge note recorded, and it is stated again
rather than quietly inherited.

**`display_scale` (0.5×–2.0×): the cursor does not scale, and did not before either.** Measured at
0.5×/1.0×/2.0×: ImGui's cursor is 77 px at every one — `ImGuiStyle::MouseCursorScale` stays 1.0
and no gamescope code touches it or calls `ScaleAllSizes()`. So this is not a regression. It is
also, I think, **correct**: `display_scale` scales the overlay's own UI, but a pointer is a
system-level affordance the user has already sized once in their desktop settings. Every other
application on their desktop uses that size; having the pointer resize because one app's UI scaled
would be the surprising behaviour. If the user disagrees, the fix is `MouseCursorScale`, one line.

**Config keys and formats: unchanged.** No new ConVar, no new config key.

*Cheap to reverse:* `OverlayShouldDrawSoftwareCursor()` in `src/CursorPolicy.h` is the single
place the preference is expressed. Returning `true` unconditionally restores #69's behaviour
exactly, without touching either backend.

---

## 2026-08-23 (P5 — the deletion, and the flag)

### D21 · The `overlay_e2` ConVar is REMOVED, not kept as a no-op

P5's brief allowed either: keep the flag as a no-op for one release, or remove it
outright. **Removed.**

**Why.** Once the legacy path is gone there is nothing to turn off *to*. A ConVar that
accepts `overlay_e2 0` and still draws the E2 shell is a control that renders and does
nothing — which is the exact defect class this redesign has spent five phases removing
(#25, #68, the computed-but-never-drawn `nColumns`, the declared-but-never-registered
`Kind::Meter`). Shipping a seventh instance of that smell *as the closing act of the
phase that deleted the other six* would be incoherent.

Second: a silently-dead setting is worse than an absent one. `overlay_e2 0` appearing to
be accepted while nothing changes reads as "this setting is broken", which costs more
trust than a missing command does.

**What removal actually costs, measured rather than assumed.** An unknown ConVar in
gamescope logs `Command not found.` and continues (`src/convar.cpp`) — it is not fatal.
So a stale `overlay_e2 1` in someone's launch script degrades to one warning line, not a
failed session. That asymmetry is the whole argument: the cost of removing is bounded and
visible, the cost of keeping is an invisible lie.

**Config compatibility is not in question.** D12 deliberately kept this runtime-only and
**never** a config field, so no config file has ever contained the key and none can fail
to load without it. Verified live: a seeded pre-branch `global.json` (including two keys
the schema does not know) loads unchanged and is byte-identical afterwards.

`shell.classic` — the setup.shell Action row offering "switch back" — goes for the same
reason in miniature: a button whose verb has nowhere to go is a button that lies.

*Cheap to reverse:* re-adding a ConVar is three lines. What would not be cheap is
re-adding the legacy path, and that is deliberate.

### D21.1 · `Kind::Meter` is REGISTERED, not deleted

The pre-P5 report (§7.2) left this open as "either register it or drop the kind".
Registered, as `display.budget_meter` in the frame limiter's Diagnostics group.

**Why build rather than delete.** SPEC §3.8 does not merely declare the kind, it *names
the instance*: "`display.budget_meter` is the drawn instance". And D20.2 had already set
the precedent one day earlier by building the multi-column sheet rather than deleting the
column count that computed it. Deleting a kind the spec specifies, in the same phase that
built the thing the spec asked for, would be two different answers to the same question.

**A percentage of budget, not milliseconds** — a Meter's range is fixed at registration
and the frame budget is not (it is the FPS cap when one is set, the output's refresh
interval otherwise), so a millisecond range would be wrong the moment either changed.

*Found by registering it:* the palette printed a **blank** value for the row, because
`PaletteValueText()` special-cased Composite and Facts and fell through to the binding
for everything else — and a Meter has no binding. That is the same bug D19.7 fixed for
composites. Fixed with one shared `MeterValue()` both the sheet and the palette call.

### D21.2 · Deleted by call graph, and `Chrome.cpp` was sorted before it was removed

The brief's warning was correct: `Chrome.cpp` owned both the dock and things the shell
uses. It was **not** deleted as a file until its contents were sorted by caller.
`palette::g_LiveTheme` and `EnsureThemeLoaded()` had a live caller and **moved to
Palette.cpp** — the file that owns the theme they load, and where the storage arguably
should have been all along (it was *declared* in `Palette.h` and *defined* in
`Chrome.cpp`). Two lines inside the loader were legacy-only and went with the dock:
`ConfigWindowsResizeFromEdges` (only ever mattered for resizable panel windows) and the
`panel_geometry` read. **The config key is untouched** — existing files still load, the
value is simply no longer consumed.

*The proof the coupling is really gone:* `tests/test_overlay_atoms.cpp` carried its own
definition of `g_LiveTheme` purely to avoid linking Chrome.cpp's 1600 lines into a
headless test binary. That stub is now deleted and the test shares the product's real
definition.

---

## 2026-08-23 (last, closing the three pre-P5 gaps)

### D20 · Three calls taken closing the gaps the pre-P5 test pass left open

The three items are the rail icon set (§7.1 of the test report — *and the user's own
critique point, verbatim: "Use bigger and better looking icons for the left sidebar"*),
the multi-column sheet (§6, the seventh "renders but does nothing"), and the
Reachability Law's unbuilt mechanism (§7.3). Build clean, **68/68** meson tests plus
**six** new cases — two in `overlay_ui` (the icon set) and four in `overlay_shell`
(the column geometry).

**D20.1 · The eleven rail icons are DRAWN from a data table, not baked, and the
geometry is imgui-free.** SPEC §8.0 specifies eleven inline-SVG glyphs on a 24-unit
grid; the rail drew the area title's first character, and at ladder step ≥ 1 — where
the label disappears and the mark carries the item's entire meaning — `Mixer`/`Monitor`
were both `M`, `Profiles`/`Per-game` both `P`, `Shaders`/`Shell` both `S`.

*Chose:* a new `Icons.h`/`Icons.cpp` holding the eleven paths as a `constexpr` table on
the 24-unit grid, and one `glyph::RailIcon()` in `Controls.cpp` that turns a shape into
`ImDrawList` calls and **contains no coordinates of its own**. Drawing rather than
baking is the established pattern here (D18's chevron, magnifier and lock) and the
argument is stronger for these: a bar chart and a droplet have no code point a wider
font range could reach, SPEC §8.0 forbids an icon font in the same breath as the
external asset, and the atlas is rebuilt per effective scale so every baked range is
paid again at every scale change.

*Why the data is imgui-free:* the same reason `Lane.h` and `Layout.h` are — the
geometry is the part worth testing, and a test should not need a graphics context to
ask whether all eleven exist, stay inside their box, and differ from one another. The
anti-collision test fails the build naming the colliding pair; mutation-checked both
ways.

*Rejected — adding an `Icon()` declaration to `Area`.* It is the tidier model, but it
would edit six panel files that are P5's deletion targets, for no behavioural gain. The
lookup is keyed by area id in one file instead, and a **missing icon falls back to the
initial** rather than to a blank rail — so a forgotten glyph degrades to the old
behaviour for one item.

*One glyph has no mockup original.* `index.html`'s eleventh area is `display.output`,
which this build does not register; this build's eleventh is `setup.shell`, which the
mockup never drew. Drawn as the shell itself — a framed window with a rail down its
left edge — which is what the area configures and what keeps it clear of Shaders' stack.

**D20.2 · The multi-column sheet is BUILT, and the unit of packing is a GROUP.**
D19.9 left `nColumns` computed, printed and consumed by nothing. It is now consumed.

*Chose:* one `LayOutSheetColumns()` in `Layout.cpp` that is **the only place a sheet
column's geometry is decided**, using index.html's own formula
(`colW = (sheet − 2·pad − (cols−1)·gutter) / cols`), and a greedy balance that assigns
each **whole group** to the currently-shortest column — the mockup's own algorithm
("greedy balance by row weight, not by group count"), kept identical because the mockup
is the declared tiebreaker.

*Why a group and never a row:* a group split across a column boundary either orphans
its rows under no heading or forces the band to be repeated at the top of the next
column. The first is unreadable; the second makes one declared group look like two.
Keeping a group whole costs some balance and buys the row grammar intact.

*The D17 interaction, which is the part that had to be right:* the drawer floats over
the sheet's right edge, so occlusion is computed from **each column's own right edge**
against the drawer's left edge. That reduces to exactly D17's single subtraction when
`nColumns` is 1 — pinned by a test — so the two are one rule rather than two that can
drift.

*Also chosen: `Solve()` gained a `bUnsplittable` flag* for an escaped legacy panel (it
lays itself out with ImGui's own cursor) and an area with a content body (one scrolling
list cannot be cut in half). Decided in `Solve()` rather than at the drawing site
**because `shell.layout` prints `Solve()`'s number** — answering it anywhere else would
put back the very defect this removes. Verified live: `system.log` reports `1 col` and
`system.monitor` `3 col` at the same 0.5×, and both match the screen.

**D20.3 · The Reachability Law's mechanism is BUILT rather than the spec being
rewritten to match the code.** SPEC §6.3 says a row that owns Params renders them
inline in the Sheet whenever the Inspector is unavailable. It was never built, and a
comment above `DrawExplainPage()` claimed it was — which is how the next reader
concludes a law is covered when it is not.

*Chose:* build it. The spec commits to inline expansion in four places (§6.3's
three-clause argument, §8.2's key table twice, §6.4's "honest cost", §2.4's amendment
about Params-in-Sheet), and §6.3 clause 1 asks for exactly one property — "one code
path" — which `DrawInlineParams()` satisfies by allocating a `RowCtx` from the same
lane and calling the same `DrawSharedControl()` the sheet row and the Inspector both
call. Rewriting the spec would have meant weakening a law to match an omission.

*A spec ambiguity resolved, and recorded because it is a real choice:* §8.2 gives
Left/Right three jobs at once — adjust, cross a region edge, expand/collapse — without
saying which wins on a row that is both, and most rows owning params also own an
adjustable control. **Adjusting wins; expansion takes the leftover**, reusing the
"didn't move" signal the region-edge rule already runs on. The alternative would let a
registration change (giving a slider a parameter) silently stop the arrow keys from
changing that slider.

*One expansion at a time.* §6.4 concedes the reflow §8.3 otherwise forbids, and accepts
it only because it is user-initiated; a single open row bounds that reflow to one place.
Esc's ladder also names "inline expansion" in the singular, and that rung now has
something behind it.

*The comment was not merely edited.* It now says what is true and records that it used
to lie, so the correction is auditable rather than invisible.

---

## 2026-08-23 (the pre-P5 test pass)

### D19 · Nine calls taken while testing the E2 shell exhaustively before P5 deletes the old UI

Full write-up, with counts and evidence:
`round-2/e2-inspector-plus/SHELL-TEST-REPORT.md`. Build clean, **68/68** meson
tests plus one new `overlay_atoms` case.

**D19.1 · A registry getter may not touch ImGui, and the fix is a published surface
size rather than a guard.** `shell.layout`'s Facts summary is `FormatLadder()`, which
called `ImGui::GetIO()`; `overlay_e2_palette query shell.` runs it on the console
thread, where there is no context, and gamescope aborted. The obvious repair is
`if ( !ImGui::GetCurrentContext() ) return {}` at each site. **Rejected:** it makes
every future getter's author responsible for remembering, which is the same process
defence SPEC §5.2 exists to replace. Chosen instead: `Draw()` publishes the surface
size into two atomics once a frame and every reader takes it from there, so the
question "does this getter have a context" stops being askable. This is D15's
`fonts::RebuildAll()` lesson applied to the read side.

**D19.2 · Esc disarms, and so does moving the selection — chosen over a shorter
timeout.** An armed `Delete saved config` survived both, so arm → Esc → re-select →
one press deleted the file inside the 6 s window. Shortening `kArmTimeout` would have
narrowed the window without closing it, and would have made the *honest* two-press
path flakier. The disarm lives in `Select()` (every selection route goes through it)
and unconditionally at the top of the Esc ladder (Esc is the user saying "no", before
any region bookkeeping).

**D19.3 · An over-wide chip bank is SCALED into the lane, not right-bound out of
it.** SPEC §2.2 makes the right edge invariant and `Bank()` broke it. Three repairs
were possible. *Right-binding the full run* pushes the first chips out of the lane's
left side and under the label — worse, because the chips that vanish are the ones
nobody would look for. *Downgrading to a dropdown like `Choice()`* is wrong for a set:
a dropdown shows one value, and a bank's value is several. *Scaling* keeps every chip
inside the lane, hit-testable, and proportional; the text clips inside the narrower
cells, which is the same loss `DrawText`'s left-align fallback already accepts
elsewhere. At 1.0x nothing scales and nothing moves.

**D19.4 · The bank gets a chip cursor rather than a new kind of control.** A Bank was
the only control in the product with no keyboard route at all — `AdjustValue()`
refuses the kind, and there is no popup — so `log.sources` and `log.severity` were
pointer-only, in a project that forbids synthetic pointer input and therefore could
not test them either. The cursor is one shell int (`s_nBankChip`), reset with the
selection exactly like `s_nInspectorFocus`, and it reuses SPEC §8.2's existing verbs
unchanged: Left/Right is "inside a control: adjust", Space/Enter is "activate /
toggle". **Rejected:** a bank-specific popup list, which would have introduced a
second modal surface and a second set of keys for one kind.

**D19.5 · The focus ring is the atom's job, and it is SPEC §7.3's measured value.** A
cursor nobody can see is not a cursor, so `Bank()` takes the focused index and draws
the ring itself — the alternative, letting the shell paint a rect over the atom's
output, is the drawn-vs-hit-tested divergence `Controls.h` exists to prevent. The
colour is `Accent @ 85%`, which §7.3 already measures at 6.49:1, so no new token.

**D19.6 · SPEC §2.4's affordance column is BUILT, not deleted from the spec.** It had
had no call sites since P1 (D18's own still-open list), so a reviewer could reasonably
have concluded the design had dropped it. It is kept because with the Inspector hidden
the chevron is the sheet's only sign that a row has depth at all — the Reachability
Law's discoverability half. The lock is a **drawn** glyph on D18.2's reasoning: no
bundled Geist face carries U+2337, so a wider baked range could not have produced it.
The column is **suppressed inside the Inspector**: a chevron there would point at the
depth you are already standing in.

**D19.7 · A composite's value is its resolved string everywhere it is printed.** The
palette and Details' binding grid read the A-axis binding and showed `0` / `8116985`
two lines from a band reading `top-right · 32 / 32`. `CompositeValue()` already
existed and is now the single answer, so there is no second formatting of the same
declaration to drift.

**D19.8 · `overlay_e2_select` goes through `Select()`.** It had duplicated `Select()`'s
body minus the Inspector focus reset and the explain-page close — a second selection
path, which meant every keyboard test driven from the console was exercising a state
the product never reaches. Recorded as a decision and not just a fix because it is the
testing tool's own correctness: a console selection that is not a real selection
quietly invalidates the evidence gathered with it.

**D19.9 · The multi-column sheet is REPORTED, not built.** `Solve()` computes
`nColumns` (3 at 0.5x, 2 at 0.75x/1.0x/1.75x for a 25-row area), `shell.layout`
prints it, and `DrawSheetBody()` has one y-cursor and draws one column at every
scale. That is the seventh instance of this branch's dominant defect class, and it is
the largest one — but building a balanced multi-column sheet touches group bands,
composites, the content body, selection and the clipper, which is a phase of work
rather than a test-pass fix, and guessing at it here would have repeated exactly the
mistake D16.2 declined to make. Left for P5 with the evidence attached. The readout
was deliberately **not** patched to print `1`: an honest number that disagrees with
the screen is what made this findable.
>
> **Superseded by D20.2 (2026-08-23, same day).** It was built. The readout and the
> screen now agree at every scale, and the honest number is what made that checkable.

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

> **Corrected 2026-08-24, by the user directly** — see the dated block at the top of this
> file. The "joined the areas that already own those concerns" placement above is exactly
> what the user objected to; VRR, Allow tearing and Force grab cursor now live in a new
> `display.general` area instead.

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


---

## 2026-08-23 (later still)

### D18 · The three deferred defects, and the four more that closing them uncovered

D17's lane fix, the glyph decision, and P4's five keyboard gaps. Build clean, **68/68
tests**, nine new cases (`overlay_ui` 47 → 54, `overlay_shell` 19 → 21). Full write-up:
`round-2/e2-inspector-plus/IMPLEMENTATION.md`.

**D18.1 · The 2.0× lane clamp is ONE line, placed before every derivation.** D17 chose the
lane; the only open question was *which* lane quantity gives way. Holding `Lw` at its
full-width value and pulling in only the control zone — the obvious reading of "the right
edge moves" — **does not work**: at 2.0× `Lw` is 348 and the visible width 368, so the
control zone would be 20 units, i.e. zero after the affordance. So the occlusion is
subtracted from `flWidth` at the *top* of `ForColumn()` and everything else — `Lw`, the
control zone, the affordance — follows from the reduced width. The occluded case is
therefore the *same arithmetic* as the ordinary one rather than a second branch that can
drift. `rcCol` takes its right edge from the lane too, so group bands and a content body
retreat with the rows instead of sliding under the drawer. **Cost, as D17 predicted:** the
control zone is 128 base instead of 368, so a long-labelled segmented control downgrades to
a dropdown sooner. **Verified by mutation**, not by reading: with the clamp disabled the
control column's right edge is 1624 px against a drawer starting at 928.

**D18.2 · Glyphs are DRAWN, and the deciding fact is not a preference.** The brief offered
"extend the baked range" or "draw shapes". Extending the range **cannot work for two of the
three marks**: the bundled Geist faces — all five, Sans and Mono — have no U+25B8 (`▸`) and
no U+2315 (`⌕`) in their cmap at all, so a wider range would bake a fallback box just as
faithfully as a narrow one. The choice was never atlas-vs-draw; it was *draw, or ship a
different typeface*. Drawing also costs nothing at bake time, which matters here because
the atlas is rebuilt per effective scale (#38).

**D18.3 · The sweep found ONE box glyph, and the brief's premise was half wrong — recorded
rather than quietly corrected.** `›` (U+203A) in the spine's `"inspector ›"` was the only
non-Latin-1 character the shell could emit. **`▸`, `⌕` and the arrow glyphs are not in the
C++ at all** — they live in `index.html`, and the depth affordance `▸` belongs to is
unimplemented (`RowCtx::Affordance()` has *no call sites*, which is its own finding).
`…` and `→` appear only in comments; `™` in `main.cpp` is terminal `--help` output, not the
overlay. The knock-on benefit of drawing: the palette's `>` prompt and the dropdown's
lowercase `v` caret — two letters pretending to be icons, both introduced *because* of the
range — became the marks they were standing in for.

**D18.4 · The range stops being a comment and becomes callable.**
`fonts::FirstUnbakedCodepoint()` turns "is this string drawable" into a question, and
`overlay_e2_glyphs` asks it of every registered area title, row and param name, help
sentence, option label and unit — strings declared across a dozen files that no fixture
sees. **Live answer today: clean.** Malformed UTF-8 is *reported*, not skipped, because
silently ignoring it is how a mojibake label survives review. *Why a console command and not
a test:* same argument as D16.8 — those strings only ever coexist in the live registry.

**D18.5 · One index covers the whole Inspector, and the mode strip is index −1.** Rather
than a focus flag per thing in it: −1 the strip, 0 the entry's own row, 1..n its params.
The strip is not a separate mode because SPEC §8.2 *already* says what arrows do ("move
selection within the focused region"; "inside a control: adjust") and the strip **is** a
two-cell segmented control — so Up onto it and Left/Right across it is the existing grammar,
with no new key to learn or document. Left/Right there takes its value from the direction
(D16.6's rule), so holding a key settles.

**D18.6 · A Choice opens its popup from the keyboard only when it ACTUALLY drew as one.**
`controls::Choice` decides segmented-vs-dropdown by measuring against the lane it got, so
the answer depends on font, scale and drawer. The shell must not re-derive it — a second
copy of that decision is precisely the drawn-vs-hit-tested divergence `Controls.h` exists to
prevent — so the row painter *records* which ids drew as dropdowns and the keyboard reads
the previous frame's record. **Without it, Enter on a segmented Choice would set the open
state, no list would be drawn, and the popup handler would swallow every key with nothing on
screen** — a keyboard trap, worse than the gap it closed.

**D18.7 · REAL KEY EVENTS ARE NOW SENDABLE, and this is NOT what D4 bans.** P4 had to
report that no real keypress had ever been sent. `overlay_e2_key` appends to
`s_InputQueue` — the overlay's own producer/consumer queue, the one
`wlserver_dispatch_key()` writes to — so a key takes the *identical* path a physical press
takes: `DrainInputQueue()` → `HandleKeyEvent()` → `ImGuiKeyForKeycode()` →
`io.AddKeyEvent()`. **It cannot reach any window, client or seat outside this overlay,
because nothing else reads that queue.** D4 bans injected input because `ydotool` drives the
whole *seat* and anything focused receives it; that hazard is structurally absent here.
There is no parallel input path to keep in step, which is the property that makes a binding
verified this way verified *through its real mechanism*. Every keyboard claim in this block
was checked by pressing the key.

**D18.8 · Four defects that only a keypress could find.** Each had been invisible for
phases, and each is the "renders but does nothing" class this branch keeps meeting.
*(a)* **`KEY_SLASH` was absent from `ImGuiKeyForKeycode`**, so `Ctrl+/` produced no
`ImGuiKey` at all — unreachable from a *real keyboard*, not merely from a test. Punctuation
still typed fine through `AddInputCharactersUTF8()`, and that asymmetry is what hid it: `/`
worked everywhere it was **typed** and nowhere it was **bound**. *(b)* **The dropdown list
never rendered at all.** ImGui closes a popup whose parent window is not focused, and the
slab carries `NoBringToFrontOnFocus` *by design*, so `OpenPopup`/`BeginPopup` opened and
closed it every frame. It is now drawn by the shell in a sibling window — the same shape,
and the same reason, the palette already documents. Nobody had ever seen this because
opening a dropdown needed a click, which this project may not synthesise. *(c)* **Left/Right
was a dead key on the sharpness slider**: its binding has 21 notches (raw 0..20) behind a
declared `0..100` range, so the default step of `(hi-lo)/100 = 1` round-tripped straight
back to the value it started from. Declared `.Step(5)`. A *drag* never showed it, because a
drag crosses several notches at once; only the keyboard moves by exactly one. *(d)* **ImGui's
own keyboard nav was running a second focus model over the same keys**, leaving a focus
rectangle unrelated to the shell's region. E2 implements SPEC §8.2 in full, so it turns nav
off for its own frames — the same call D16.3 made about `InputText`, for the same reason.

**Still open, and NOT fixed here.** *(a)* `RowCtx::Affordance()` has no call sites: SPEC
§2.4's depth affordance is specified and unimplemented, which is why `▸` was never needed.
*(b)* The palette's footer legend overlaps its last result row at 2.0× — the panel is
clamped to the slab but the legend is placed from a fixed offset. Pre-existing, cosmetic,
untouched. *(c)* Only the sharpness slider was proven to have the quantised-binding problem;
other sliders with converting bindings may share it and were not audited. *(d)* `Escape()`
is untouched, per the brief — that is P5's.

### D17 · At 2.0×, the open drawer shrinks the sheet's lane rather than covering it

**The defect (found in P4, recorded as D16.2):** at 2.0× the ladder reaches step 2, where the
inspector is a *drawer* — an overlay pinned to the right. With the sheet 804 wide and the drawer
400, the drawer paints over the sheet's **entire control column**. Every segmented control,
switch and slider is hidden behind it. This matches `SPEC.md` §8.3's own table, so it is a design
gap rather than an implementation slip, and the previous agent correctly declined to pick a repair
that changes what the ladder means.

**Chose:** while the drawer is open, the sheet's **lane right edge** becomes the drawer's left
edge minus a gutter. Controls stay reachable; the drawer still overlays the sheet's *background*
and still opens and closes without relayout elsewhere.

**Rejected — auto-hiding the drawer when the pointer enters the sheet.** It makes the inspector
flicker during ordinary use, and it silently overrides `Ctrl+I`, so the user's explicit choice
stops meaning anything.

**Rejected — collapsing step 2 into step 1** (a real column that reflows the sheet). That is
already what step 1 *is*; the ladder would then have two identical steps, and the reason step 2
exists is to buy the sheet more width at high scale.

**Rejected — narrowing the drawer at 2.0×.** The drawer holds the same content at every scale, so
narrowing it at exactly the scale where text is largest is the wrong direction.

**Why the lane is the right lever:** it is already the single place control geometry is decided
(P1's Lane unit, `Place()`), it is imgui-free and therefore directly testable, and no call site
can observe the difference — which is precisely the property that made the lane law worth having.

**Cost, stated plainly:** at 2.0× with the drawer open, the control column is narrower, so a
segmented control with long labels may downgrade to a dropdown sooner. That is the existing,
specified downgrade behaviour rather than a new failure mode.

*Cheap to reverse:* one clamp in the lane computation, pinned by tests.
