# FPS display: margin

Margin: the measured-to-first-visible-row rule, the bottom-anchor nudge and the worst-case digit correction. Part of [fps-display](fps-display.md); sections below are moved verbatim from that page.

## Margin

**Definition:** `margin_x`/`margin_y` is the distance, in pixels, from the
screen edge to the **nearest drawn pixel** of the HUD — whatever is
outermost in the current configuration. With margin 0 that outermost pixel
touches the screen edge exactly; with margin 10 there are exactly 10 blank
pixels between the edge and it. Same rule on both axes, at every anchor.

**Which element the measurement lands on:**

| Outline | Outermost element | How exact |
|---|---|---|
| on | the outline's outer ring | Exact at the ink's true (antialiased) boundary; the *solid* ring starts one AA-fringe pixel further in — see the two-assertion scheme below. |
| off | the glyph ink | Same. |

There is no third row any more. A drawn backdrop used to be one — its
`AddRectFilled` edge took ImGui's `PrimRect` fast path with no antialiasing
fringe at all, so it landed on the margin exactly and pinned it for
everything else — and it was the only configuration with a crisp edge. It
went with the backdrop (2026-09-09), which is why the checks had to change
shape rather than just lose a case; see below.

**The bug (fixed 2026-09-07):** nothing pinned the digits' own ink to the
invisible box at all. They sat inset from it by `backdrop_padding` (6px,
always added whether or not a backdrop was actually drawn; removed
altogether 2026-09-09) plus each glyph's own **side bearing** /
**cap-height gap**: `ImFont::CalcTextSizeA()` measures the ADVANCE box (pen
cell width, full ascent-to-descent line height), not the tight box the
glyph's own ink occupies, and a digit's ink starts a little in from the
left of its cell and stops a little short of the font's full ascent — gaps
`CalcTextSizeA()` cannot see and the old code never corrected for. Measured
2026-09-07 at font size 36, backdrop off, outline off, top-left anchor
(the untracked `hud-margin-2026-09-07` capture run):

| margin | measured left | measured top |
|---|---|---|
| 0 | 7 | 14 |
| 5 | 12 | 19 |

— i.e. +7px horizontally (`padding(6) + ~1px` bearing) and +14px vertically
(`padding(6) + ~8px` of cap-height headroom the digits' own ink never
reaches). The padding half of that is gone now; the bearing half is what
`EdgeShift()` still cancels. Both **before-fix screenshots showed the readout sitting visibly
off the corner even at margin 0**, when it should have been flush.

**The fix:** `MeasureFpsModule()` measures the true ink bounding box
directly off the font's own baked glyphs — until 2026-09-14 the metric box
`ImFontGlyph::X0/Y0/X1/Y1`, since then the glyph's atlas bitmap through a
visibility floor (see *2026-09-14* below), both via `ImFont::GetFontBaked()`
(public API, no `imgui_internal.h` needed) — for **a pinned `'0'`-run
reference string** (`szPadded`), not the actual digits being drawn — the
same reference the box-*width* jitter-prevention scheme two sections up
also uses, so a value change never moves the readout by a pixel on any
axis; see *2026-09-28* below for the one bottom-edge case this leaves, and
its fix. On whichever axis the
anchor actually hugs an edge — not the centred axis, which has no margin
claim to satisfy — it shifts the digits by exactly enough to cancel that
bearing (and, until 2026-09-09, the padding with it), so the ink (or the
outline's own outer ring, when one is drawn) lands flush at the margin. The
pure arithmetic is `fpsmath::EdgeShift()` (`FpsDisplay.h`), covered by
`tests/test_fps_counter.cpp`; `MeasureFpsModule()` supplies the font's own
measured bearings and the outline's geometric reach (never less than 1px
once an outline is drawn at all, matching the sub-pixel-radius path's own
whole-pixel ring — see the Outline section above).

**The antialiasing fringe, and how the check stays exact without a
backdrop (2026-09-09).** A rendered glyph's edge is antialiased, so its true
boundary carries a fractional-coverage fringe — a pixel differing from its
flat background by a single count, right where the ink is supposed to
start. Until the backdrop was removed, the backdrop rect was the one
fringe-free case and therefore the one measured at tolerance 0, with the
ink and outline cases allowed 1px.

With every case now a fringe case, the answer was **not** to leave
everything at tolerance 1 — that accepts a real 1px drift in either
direction. `pixel-regression.sh`'s `check_hud_margin()` **splits** the
assertion instead, running both on the same capture:

- **`*-edge`** — the bounding box of every pixel that differs from the flat
  background *at all* (`diff_thresh 0`, so the sub-count fringe counts).
  That fringe pixel **is** the glyph's true geometric boundary, so this is
  asserted at **tolerance 0** — exactly the standard the backdrop rect used
  to meet, it just needed a sensitive enough threshold to see.
- **`*-ink`** — the solid ink at the usual "not the background" threshold,
  one fringe pixel further in, at tolerance 1.

Together they pin *both* ends of the fringe: the ink cannot creep inward
while the fringe alone lands right, and the fringe cannot creep outward.
`tests/test_fps_counter.cpp` pins `EdgeShift()`'s arithmetic exactly, as
before.

**Partially superseded 2026-09-29, HORIZONTAL AXIS ONLY.** The LEFT/RIGHT
bearing no longer comes from a single pinned `'0'`-run reference — the
theory that `'0'`'s overshoot is always the font's deepest turned out false
on this axis, with the outline on, at some sizes (see *2026-09-29* below).
It is now the worst case across all ten digits on that axis alone, computed
once per (font size, ink floor) and cached. **The TOP/BOTTOM bearing is
UNCHANGED** — still exactly this section's `'0'`-run reference, plus the
2026-09-28 bottom-anchor nudge below — at the user's explicit request going
into that task (see *2026-09-29*'s own section for the quote). This section
is kept as the accurate description of the vertical axis, and as history
for how the horizontal axis used to work before it needed something more.

**Measured after the fix**, and re-measured after the backdrop removal
(font 36, flat 148 background, `*-edge` — the fringe's own outer pixel):

| margin | measured left | measured top |
|---|---|---|
| 0 | 0 | 0 |
| 5 | 5 | 5 |

Verified across all four corners, all four edge-centre anchors, margins
0/1/5/20, outline off and on, and 2-/3-/4-digit readings — 152 assertions,
148 exact and the 4 flat-digit ones above. **That "exact" held only for the
one font size and the one background it was measured on** (36 px on flat
148 grey, where a faint glyph row still registers as a one-count
*darkening*); on a dark game the same row vanishes and the digits sit 1 px
in from the margin — see *2026-09-14* below. The untracked `hud-backdrop-removal-2026-09-09` capture run has the full table, the raw log and
8×-zoomed corner crops at margin 0 and margin 5, with and without the
outline. `scripts/pixel-regression.sh`'s `check_hud_margin()` keeps a
compact, permanent subset of that matrix green: all four corners at margins
0 and 8, outline off and on (the outline-on case replaced the retired
backdrop-on one), plus one 4-digit reading.

### 2026-09-14: the margin is measured to the first row that can be seen

**The report.** The user: the HUD sits 1 px too far from the bottom edge at
a bottom anchor. A font-size sweep (margin 8, outline 0, zero-tolerance diff
of the on-screen capture against a no-HUD capture of the same `dark` scene,
the untracked `fps-hud-bottom-2026-09-14` capture run, `measurements.txt`)
made it a property of the size, not the edge: sizes 12 and 14 → bottom gap
9 (top exact); 18 and 36 → top gap 9 (bottom exact); every other size from
16 to 72 exact on both. Outline 2 changed nothing about which sizes flip.
In every off case the on-screen ink was exactly **one row shorter** than
`MeasureInkExtent()` predicted, never taller — so not a whole-layer shift
(that would move both edges), but one edge row going missing.

**The cause.** The missing row is a real row of the baked glyph that cannot
survive the composite. The pinned reference `'0'` is round, so it carries
the font's cap-height and baseline *overshoot*; at each size the rasteriser
clips whatever slice of that curve falls into the last pixel row, and at
some sizes that slice is a few percent. Read straight out of the ImGui CPU
atlas (Geist Mono SemiBold, `PixelSnapH`, the shipping configuration —
peak alpha of the `'0'`'s edge rows, out of 255):

| size | top row | next row | last row | row before |
|---|---|---|---|---|
| 12 | 152 | 234 | **15** | 245 |
| 14 | 181 | 253 | **19** | 252 |
| 18 | **2** | 235 | 27 | 254 |
| 20 | 21 | 255 | 41 | 255 |
| 36 | **8** | 249 | 89 | 255 |
| 48 | 189 | 255 | 134 | 255 |

The same happens horizontally: at 26–28 px the `'0'` bakes a left column
of 7, 3 and **0** (an entirely empty column inside its metric box), at
33/40/47/54 px a right column of 3. The metric box `X0/Y0/X1/Y1` includes
all of these.

Such a row is in the HUD's own texture (a GPU readback of
`s_pOverlayTexture` shows it), but in the default configuration — Fixed
colour, no outline — it never reaches the screen. ImGui's straight-alpha
pipeline blends the glyph over the cleared texture with
`(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`, so the texel is stored as *colour ×
coverage* with alpha = coverage; the composite's
`ALPHA_BLENDING_MODE_COVERAGE` (`alphamode.h`) then decodes that colour from
sRGB and multiplies it by the alpha *again*. A white row of coverage `c`
lands on a dark game at roughly `encode(decode(c)·c)`: for 8/255 that is
+0.1 of a count over the scene's 5-grey, for 15/255 −0.3 of a count over
20-grey — both round back to the background. The model reproduces every
capture pixel-for-pixel (at 20 px the `'0'`'s 41/255 bottom row predicts
+10.2 over 5-grey and measures +10; the `'6'`'s 49/255 predicts +15 and
measures +15). The margin was being measured to a row nobody could see.

**Not the compositor.** The previous pass concluded `rendervulkan.cpp`
shifts the layer. It does not: `FpsDisplay_AddLayer()` pushes the HUD at
offset 0, scale 1, and a shift would move both edges. The data — ink one
row *shorter*, on whichever end the overshoot phase made faint — is the
signature of a faint edge row, and the atlas numbers above are that row.

**The rule.** The margin is now measured to the first row / column of the
reference glyph that can *actually show*: one whose strongest pixel can
change what is on screen by at least **1/16 of full scale** over its most
favourable background. That one criterion gives a different coverage floor
per blend path, because the three paths turn coverage into on-screen change
differently (`fpsmath::InkCoverageFloor()`, `FpsDisplay.h`):

| outermost element | blend | on-screen change of coverage `c` | floor |
|---|---|---|---|
| Fixed digits, no outline | coverage blend, colour already × coverage | `encode(decode(c)·c)` over black | **47/255** |
| the outline ring (either mode) | black stamps, straight alpha | measured on white: darkens by about `c` counts (15→13, 27→42, 54→65, 89→119) | **16/255** |
| Inverted digits, no outline | `alphamode.h` recovers `c`, applies it in linear light | `encode(c)` over black | **2/255** |

Why the floor has to depend on the mode: in Inverted mode that same 8/255
top row at 36 px lands near **49/255** on black — plainly visible — because
the invert path applies coverage as a linear-light mixing ratio, which is
what coverage is; only the Fixed path multiplies it in twice. One floor for
all three would either keep placing Fixed digits a pixel in, or start
placing Inverted ones a pixel out. `tests/test_fps_counter.cpp` derives all
three constants from the blend formulae, so a change to either the floors
or the shaders that disagrees with the other fails a test.

**The measurement.** `MeasureInkExtent()` now takes the floor and boxes the
atlas pixels at or above it — `fpsmath::ScanInk()`, a pure function over an
8-bit coverage bitmap (Alpha8 or the alpha byte of RGBA32), unit-tested on
a synthetic glyph with a faint top row, a faint bottom row, an empty left
column and a faint right column. The glyph's atlas rect comes from its UVs
(`ImTextureData::GetPixelsAt()`, CPU pixels ImGui keeps for its dynamic
atlas), and the box is mapped back onto the metric box, which stays as the
fallback for a glyph whose bitmap cannot be read. With an outline the ring
is the same bitmap stamped out by the radius, so its visible edge is the
same floor on the same bitmap and `EdgeShift()` adds the radius as before.

**Measured after the fix** (the untracked `fps-hud-bottom-2026-09-14` capture run, `after`; table in `measurements.txt`): sizes 12,
14, 16, 18, 20, 24, 36 and 48 at margins 0 and 8, outline 0 (dark scene)
and 2 (bright scene — a black ring cannot register on near-black),
top-left and bottom-left, plus bottom-right at 12 and 36. The harness
measures by the code's own rule translated to the screen — a pixel is ink
when it differs from the no-HUD baseline by at least what the floor
coverage produces on that background pixel — and prints the zero-tolerance
gap beside it with the peak change of every excluded row, so the fringe it
discounts is on record: on the dark scene those rows are at most a few
counts. 67 of the 72 visible-rule gaps equal their margin; the 5 others are
the reference-glyph limitation above straddling the floor (the `'6'`'s
bottom row is 49/255 at 20 px where the `'0'`'s is 41, its left column
50/255 at 24 px where the `'0'`'s is 29; the `'0'`'s right column at 12 px
is 17/255 against the ring's 16) — a 7–16-count fringe one pixel off,
listed in `measurements.txt`.

**What is deliberately not exact.** Zero-tolerance diff tools will still
find the excluded fringe one pixel *outside* the margin on backgrounds
where it registers at all (a one-count darkening on mid grey, +1 to +4 on
dark). That is the trade: the row is placed where a person would say the
digits start. `scripts/pixel-regression.sh`'s `check_hud_margin()` used to
assert its `*-edge` box at tolerance 0 on flat 148 grey, where the 36 px top
row showed as exactly that one-count darkening one pixel outside the
margin — not changed in this pass, which owned only the HUD's own files.
**Fixed 2026-09-14 (separate pass):** `*-edge` now applies the same
visibility floor, converted to a pixel-count delta for its own
colour/background via `pixel_regression_sample.py`'s
`coverage_blend_expected()` (`HUD_MARGIN_DIFF_EDGE_DIGIT`/`_OUTLINE` in the
script) — see that script's own comment above `assert_hud_margin()` for the
derivation.

### 2026-09-28: bottom anchors get a fixed +1px nudge

**The report.** The user: at a bottom anchor with margin 0 the HUD sat 1px
above the true bottom edge; top anchors were exact.

**Rejected approach.** A same-day fix measured the edge-hugging ink bearing
off the actual drawn digits (`L.szNum`) instead of the pinned `'0'`-run
reference (`szPadded`), since `'0'` turned out not to be reliably the
font's deepest-overshoot digit on every axis. That fixed the bottom-edge
case, but made the readout's Y position change by up to 1px whenever the
displayed value crossed between digits of different overshoot depth.
Reverted the same day — the user, verbatim: *"now the fps just moves
around depending on the value, which is definitely wrong. The way it was
previously was almost right, you just had to add a single pixel offset,
hardcoded, no logic changes needed."*

**The fix.** `MeasureFpsModule()` is back to measuring off the pinned
`'0'`-run reference, as before. `DrawReadout()` instead applies a constant
`kBottomAnchorNudgePx = 1.0f` to the resolved origin's Y, for the three
bottom anchors only (bottom-left, bottom-centre, bottom-right) — top and
middle anchors, and the left/right edges, are untouched.

The gap is an *empty pixel row inside the box*, below the ink, not a
misplaced box — `boxSize.y` already includes it, and at margin 0 the box's
own bottom edge already sits exactly on `ResolveAnchoredOrigin()`'s own
bound (`ioDisplay.y − boxSize.y`). A first cut of this fix nudged
`origin.y` down and then re-clamped it to that same bound, which just
clamps the nudge straight back out — the box cannot move because it is
already at the wall. The bound itself has to widen by the nudge amount
(`std::max( 0.0f, ioDisplay.y − boxSize.y ) + kBottomAnchorNudgePx`),
deliberately letting the box's own bottom edge sit 1px past the screen
edge, since it is the empty row — not the box's position — that needs to
leave the visible area.

`Why:` a hardcoded, value-independent offset is stable by construction,
where measuring the live digits inherently is not — the two are not
interchangeable fixes for the same report.

**Not fixed here.** The outline-on residual noted above (a handful of
(font size, digit, axis) triples landing 1px off with the outline on) is
untouched by this reversion — it was already present before, during and
after the rejected approach above.

**Still exactly this, as of 2026-09-29.** `kBottomAnchorNudgePx` stays —
see that day's own section below. A first cut of that day's horizontal fix
extended the same worst-case-digit scan to this axis and dropped the
nudge; caught in review before merge because the user had separately
confirmed this exact bottom-anchor behaviour was already correct
("The FPS display looks perfectly fine"), and that task's own report was
specifically the *side*, not the bottom. Reverted back to precisely this
section, byte-for-byte.

### 2026-09-29: the RIGHT/LEFT margin correction is the worst case across all ten digits, cached once per setting

**The report.** The user, on the outline-on residual noted above (14 at
14px, and generally): *"I just saw the one pixel gap towards the side on
certain font sizes. I actually want you to fix that, but make sure that
the font or like the text doesn't shift around like it did in an earlier
attempt to do so. So the proper position should be calculated once and
then just used. Instead of actively calculating it, so if it's wrong once
then just stays wrong, that's fine, but at least it's not annoying and
shifts around like I did earlier."*

**Scope: LEFT/RIGHT only, by explicit correction.** A first cut of this
fix (reviewed before merge) applied the same worst-case-digit scan to
*both* axes and dropped the 2026-09-28 bottom-anchor nudge. That reopened
approved behaviour: immediately before this task, on the bottom edge
specifically, the user had said, verbatim, *"The FPS display looks
perfectly fine"* — and this task's own report was specifically *"the one
pixel gap towards the **side**"*, i.e. left/right, not the bottom. The
first cut's own jitter table (still on that mixed build) showed
`bottom=25` at margin 24 for the common case (no `8` in the displayed
digits) where the approved 7d5c689 nudge read `bottom=24` — a fresh 1px
gap on an axis nobody had asked to change. Caught in review, reverted: the
vertical bearing and `kBottomAnchorNudgePx` are back to byte-for-byte
7d5c689 (`git diff 7d5c689 -- src/Overlay/FpsDisplay.cpp` shows the
vertical code paths identical), and only the horizontal bearing uses the
new scan. Every measurement below is from that corrected build.

**Reproduction.** A font-size sweep (10–48px, margin 24, top-right and
bottom-left anchors, outline off and `HUD_OUTLINE_STRENGTH` on, values
12/14/88 forced) against the pre-fix binary
(`scripts/pixel-regression.sh HUD_SIDE_SWEEP=1 --only hud-side-bearing`;
evidence in the untracked `hud-side-2026-09-29` capture run, in whichever
build dir the run used) found the single-`'0'`
reference wrong in **both directions** on the RIGHT edge, not just as a
gap: at margin 24 the measured right edge was 25 (a 1px gap, e.g. size
24/outline off) at some sizes and 23 (the outline genuinely **past** the
margin) at others (e.g. size 28/40/44 with the outline on, value "14" —
the exact case reported):

| size | outline | axis | pre-fix measured | expect | defect |
|---|---|---|---|---|---|
| 24 | off | right (value 12/88) | 25 | 24 | 1px gap |
| 28 | on | right (value "14") | 23 | 24 | 1px **overflow** |
| 40 | on/off | right (value "14") | 23 | 24 | 1px overflow |
| 44 | on | right (value "14") | 23 | 24 | 1px overflow |

Root cause, confirmed via a targeted debug dump of every digit's own ink
extent (`MeasureInkExtent()`) at size 14/outline floor 16: digit `'4'`
measures `ext.right = 7.00` against its own `6.00`px advance — its ink
genuinely overshoots its own glyph cell by 1px at that size with the
outline on, something `'0'`'s round shape never does. `'0'` was never
reliably the deepest-reaching digit on this axis at every size.

**The fix.** `MeasureFpsModule()`'s LEFT/RIGHT bearing no longer comes from
the single reference glyph. `CachedWorstCaseDigitBearingsHoriz()`
(`FpsDisplay.cpp`) scans each of `'0'`..`'9'` individually through the same
`MeasureInkExtent()` / `fpsmath::ScanInk()` visibility-floor machinery the
single-reference scheme already used, and takes the **minimum** bearing on
each of the two horizontal sides — the smallest per-digit "gap from ink to
cell edge" is the digit that reaches furthest on that side, so shifting by
that amount is the largest shift ANY digit could ever need on that axis,
and every shallower digit lands with its own true bearing minus that
amount of extra room — never negative, i.e. **never past the margin**.
Cached by `(font, font size, ink floor)` — ink floor already encodes both
outline on/off and Fixed/Inverted colour mode
(`fpsmath::InkCoverageFloor()`) — so a slider release or a mode toggle
recomputes it once; every other frame, and every value change, just reads
the cache. The outline's own geometric radius is still added afterwards
exactly as before (`flOutlineGeomRadius` in `EdgeShift()`) — only the
*glyph* bearing input changed, and only for left/right.

This is what makes the horizontal position value-independent by
construction, not by convention: the ten-digit scan never looks at what is
actually being *displayed* (`L.szNum`), only at the font/size/floor, so
there is no code path left that could read the live value into the
horizontal margin correction — unlike the rejected 2026-09-28 attempt,
which measured `L.szNum` directly (on the vertical axis, which this fix
does not touch at all).

**Measured after the fix** (same sweep, corrected binary): the RIGHT edge
at every sampled (size, outline) combination now lands at exactly 24
(flush, the worst-case digit) or 25 (1px inside, a shallower digit) —
**never 23**, across the full 10–48px sweep, outline off and on, values
12/14/88. The LEFT edge (measured via the mirrored bottom-left-anchor
sweep) shows the same 24-or-25-never-below pattern.
`scripts/pixel-regression.sh`'s permanent `hud-side-bearing` check pins
this at the sizes the pre-fix sweep actually failed at (20, 28, 36, 40px,
right/top edge pair, outline on, value "14") with `tol=1` on the whole
pair — loosened from the `hud-margin` check's usual `tol=0` deliberately,
since a 1px "shallower digit sits inside" reading on the RIGHT edge is now
the documented, accepted outcome, not a bug; what `tol=1` still catches is
a return of the 23-or-below overflow. (The TOP half of that same pair is
the untouched vertical axis, along for the ride only because the sampler
checks both edges of a corner in one call — see below for its own,
separate story.)

**No jitter, proven — twice.** First, at margin 24, bottom-left anchor,
font 36px, outline strength 2 (settings held fixed), forced through
`fps_display_force` at 1, 12, 14, 88 and 188 in sequence, each captured
and measured with `pixel_regression_sample.py`'s `margin` command:

| forced value | leading digit | measured left | measured bottom |
|---|---|---|---|
| 1 | `1` | 24 | 25 |
| 12 | `1` | 24 | 25 |
| 14 | `1` | 24 | 25 |
| 88 | `8` | 24 | 24 |
| 188 | `1` | 24 | 24 |

`left` — the pen's own X origin for a left-hugging anchor, set once by
`MeasureFpsModule()`/`EdgeShift()` from settings alone — is **bit-for-bit
identical (24) across every value**, including the jump from a leading `1`
to a leading `8` and back, and across the 2-vs-3-digit width change from
"14" to "188" (both fit the pinned 3-cell box, so the box itself does not
resize either — see "No layout jitter" above). `bottom` (the untouched
vertical axis) varies by exactly 1px between values whose digits include
an `8` (24, flush) and values that don't (25) — the SAME `'0'`-reference
behaviour this axis has always had, unrelated to this fix, included here
only to show it did not regress.

Second, at margin 0 (bottom-right anchor, plain HUD — the profile's
existing state, unmodified), the same five values measured **exactly
0/0 on both axes, with zero variance whatsoever** — see
the untracked `hud-side-2026-09-29` capture run (`jitter-v2-bottomright-*.png`).
Together the two runs show the pen origin never moves regardless of
displayed value; only the antialiased edge of whichever specific digit is
drawn can differ, by construction never more than the ten-digit spread
already measured into the cached horizontal bearing (and, on the
untouched vertical axis, never more than `'0'`'s own known spread, exactly
as before this task).

**Cross-check: `hud-margin` unaffected on the flush (non-outline) axis,
loosened by design on the RIGHT/LEFT outline axis, and one pre-existing,
unrelated gap found on the BOTTOM axis.** `check_hud_margin()`'s own
outline-on `*-edge` assertions (font 36px, "60" forced, tol 0) now read 1px
inside on LEFT/RIGHT at several corners (`'6'`/`'0'` are not the
worst-case digit at that exact size) instead of exactly 0/8 — expected
under this fix, and `assert_hud_margin()` was updated the same day to use
the `*-ink` tolerance (1px) for the outline `*-edge` case, with the
reasoning recorded next to it. TOP and BOTTOM in that same assertion are
the *untouched* vertical axis and read exactly as `git show 7d5c689`'s own
code does — confirmed by literally building `7d5c689` unmodified and
running the same check against it.

That direct comparison surfaced something this task did **not** cause and
is **not** fixing: `hud-margin-bottom-left-m8-edge` and
`hud-margin-bottom-right-m8-edge` (tol 0, non-outline, margin 8) read
`bottom=7` instead of 8 on **both** the corrected build and a pristine,
unmodified `7d5c689` build — i.e. this specific case was already off by
1px on the approved commit itself, before any of this task's work. Flagged
for a separate look; `hud-margin`'s own `*-ink` companion (tol 1) already
covers it, so this is a tol-0 cosmetic gap in test strictness, not
evidence of a rendering bug newly introduced here. The non-outline
`*-edge` assertions at margin 0 (`hud-margin-bottom-*-m0-edge`) remain
exact on both builds.


