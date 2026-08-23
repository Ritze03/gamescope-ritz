# E2 shell — the exhaustive pass before P5 deletes the old UI

Date: **2026-08-23**. Branch: `feature/overlay-e2`.
Fixes: commit `885242e`. Decisions taken without the user: `../../AUTONOMOUS-DECISIONS.md` **D19**.

This is the evidence that P5 can safely delete `Chrome.cpp`'s floating-window layer and
the six legacy panel bodies. It is written to be **checkable**, not reassuring: every
claim below names the mechanism that produced it, and everything that was *not* tested is
listed in §9 rather than left to be inferred.

**Method, in one line:** the shell was driven live — 23 nested gamescope sessions, 85
`grim` captures bounded to the session's own window — through `overlay_e2_key` (real key
events on the overlay's own input queue, D18.7), `overlay_e2_select`, `overlay_e2_set`,
`overlay_e2_palette`, `overlay_e2_host` and `overlay_e2_glyphs`. No pointer input was
synthesised (D4). Every session ran under `scripts/with-gamescope-lock.sh` with a
temporary `XDG_CONFIG_HOME`.

---

## 1. Headline

| | |
|---|---|
| Registrations exercised | **11 areas · 78 settings · 26 params** (the palette's own count) |
| Defects and spec gaps found | **20** |
| **Fixed** | **8** — §3, one commit (`885242e`) |
| **Left, with reasons** | **12** — §6 (one), §7 (ten), §8 (one) |
| **Closed since, by D20** | **3** — the rail icons (§7.1), the multi-column sheet (§6), and the Reachability Law's mechanism (§7.3). |
| **Closed by P5 (D21)** | **5 of the 9** — the collapsed rail's scrolling, `Kind::Meter` (§7.2), the Colour composite's arrow keys, the palette footer and the sheet legend (§7.3). **4 remain open**, listed with reasons in §7.3. |
| A seventh "renders but does nothing"? | **Yes — the multi-column sheet.** §6 — **now built, D20.2** |
| Config safety | **PASS** — §4 |
| Legacy mode (`overlay_e2 0`) | **PASS** — §5 |
| HUD parity | **PASS** — §5 |
| `meson test -C build` | **68/68**, plus one new `overlay_atoms` case (9 cases / 79 assertions in that suite) |

---

## 2. Coverage, with counts

### 2.1 Every binding's underlying state was proven to move

Not "the row updated" — the **binding**. For each settable id, two different values were
written through `Entry::Binding().Set()` (the same call a click makes) and read back; a
pair of equal read-backs is the #25/#68 signature.

| Sweep | Cases | Result |
|---|---|---|
| Every settable Entry and Param, two values each | **76 ids × 2 writes** | every one moved |
| Read-only kinds must refuse | **19 ids** (12 Facts, 3 Graph composites, 4 Actions) | all refused with the kind named |
| Regression re-run after the fixes | 11 ids × 2 | all moved |

The single "stuck" result was `config.override` in a session with **no app id**, where
`DisabledUnless` is showing *"no game was identified for this session"* — the setter runs
and correctly declines. Re-run with `GS_RITZ_APPID=424242` it moves, and moves the file
(§4.3).

### 2.2 Every control kind, driven by a real keypress

Counts are `.Kind(` **declaration sites** in `src/Overlay/`; several are inside helper
loops, so the live row count is higher (the seven `monitor.mod_*` switches, the four
`monitor.color_*` composites, the five `monitor.stats_*` graphs and one row per live
PipeWire stream all come from one declaration each).

| Kind | Declaration sites | Instances driven | Keyboard verdict |
|---|---|---|---|
| Switch | 13 | 20 | `Space` toggles, `←/→` set off/on by direction — ✅ |
| Slider | 13 | 19 | `←/→` step; `display.sharpness` steps by its declared 5 (D18.8c) — ✅ |
| Stepper | 1 | 1 (`display.fps_limit`) | `←/→` step — ✅ (`0 fps → 10 fps`) |
| Choice, segmented | 7 | 6 | `←/→` step, stop at both ends — ✅ |
| Choice, dropdown | same 7, downgraded by lane | 2 | `Enter` opens, `↑↓` highlight, `Enter` commits, `Esc` dismisses — ✅ (verified at 2.0×, where `display.filter` downgrades) |
| Text | 2 | 2 | `Enter` begins entry, characters type, `Enter` commits — ✅ |
| Bank | 2 | 2 | **was dead on every key in every region** — fixed, §3.4 |
| Action | 7 | 6 | `Space`/`Enter` invokes; a `Confirm()` action arms first — ✅, and see §3.2 |
| Facts | 12 | 12 | read-only, refuses — ✅ |
| Meter | **0** | 0 | never registered anywhere; see §7.2 |
| Composite | 3 | 12 (Anchor 1, Hue 1, Colour 4, Graph 6, Strip **0**) | Anchor/Hue/Colour adjust; Graph refuses — ✅, with a caveat in §7.3 |
| Param | 22 | 26 | all 21 static ones walked and adjusted; the audio `mute` params come one per stream |

**All 21 registered Params are keyboard-reachable and adjustable** — verified one at a
time with `Tab` + *n* × `↓` + adjust, with the focus **region normalised before each
case** (an earlier sweep that skipped this produced six false "dead" results; the region
is not reset by a selection, and rightly so). The two that legitimately do not move on
`→` are `adaptive_brightness.strength` and `.max_gain`, both already at the top of their
range.

### 2.3 Scale × host

The responsive ladder was read out of `shell.layout` at all seven scales for a 25-row
area and a 6-row one, and it matches SPEC §8.3's table **exactly**:

| Scale | rail | inspector | sheet | step | SPEC §8.3 |
|---|---|---|---|---|---|
| 0.5× | 232 | column 400 | 1728 | −1 | ✅ |
| 0.75× | 232 | column 400 | 941 | 0 | ✅ |
| 1.0× | 232 | column 400 | 928 | 0 | ✅ |
| 1.25× | 232 | column 400 | 750 | 0 | ✅ |
| 1.5× | **60** | column 400 | 692 | 1 | ✅ |
| 1.75× | 60 | **drawer** 400 | 927 | 2 | ✅ |
| 2.0× | 60 | drawer 400 | **804** | 2 | ✅ |

Drawn and inspected: 7 scales on `system.monitor` (25 rows, six group bands, two
composite kinds, a 6-param Inspector) plus the three Inspector hosts at 0.5× and 2.0× on
`system.log` (two banks, a Text row, a Facts row and a clipper'd content body), and the
three hosts at 1.0× and 2.0× on the Monitor. **One overflow was found** (§3.3) and fixed;
the collapse ladder, the icon rail, the spine and the drawer's lane clamp (D17) all
behave. Nothing else escaped a lane, overlapped, or clipped a control.

### 2.4 Every area

All eleven were opened, walked with the arrows, and had a row selected in each of the
three Inspector hosts: `display.upscaling`, `display.frame_limiter`, `display.hdr`,
`image.shaders`, `audio.mixer`, `system.monitor`, `system.log`, `setup.profiles`,
`setup.pergame`, `setup.appearance`, `setup.shell`. `audio.mixer` was additionally
exercised as a **dynamic** area (3 live PipeWire streams, rows rebuilt from node ids) and
`audio.volume` — a conditional row present only when a stream is detected — was reached
by pinning a stream first.

---

## 3. Defects found and fixed

Ranked by severity.

### 3.1 `overlay_e2_palette query shell.` aborted the compositor — **critical**

`shell.layout`'s Facts summary is `FormatLadder()`, which called `ImGui::GetIO()`. Every
registry getter is reachable from the **console thread**, which has no ImGui context:

```
gamescope: imgui.cpp:5244: ImGuiIO& ImGui::GetIO(): Assertion
  `GImGui != __null && "No current context..."' failed.
(EE) failed to read Wayland events: Broken pipe
```

Found on the second session of this pass, while enumerating ids. Exactly the class D15
already fixed once on the *write* side (`fonts::RebuildAll()`); this is the read side.
**Fixed:** `Draw()` publishes the surface size into two atomics; `FormatLadder()` and the
`.Live("slab")` readout take it from there and answer `"not drawn yet"` before the first
frame. D19.1.

### 3.2 An armed destructive action survived Esc and survived walking away — **high**

SPEC §3.9 says *"`Esc` disarms"*. The shell's own comment said *"walking away disarms
it"*. Neither was true: `s_sArmedAction` was cleared only by a 6-second timeout or by the
entry disappearing. Proven against a real file:

```
A: arm, Esc, re-select, ONE press   -> FAIL: ONE press after Esc DELETED the file
B: arm, walk to another row, come back, ONE press
                                    -> FAIL: ONE press after walking away DELETED the file
C: two consecutive presses          -> PASS: two presses deleted
```

**Fixed** in `Select()` and at the top of the Esc ladder. Re-run after the fix:

```
A: PASS: file survived (Esc disarmed)
B: PASS: file survived (walking away disarmed)
C: PASS: two presses deleted
```

### 3.3 A chip bank escaped the lane — **high**

`Choice()` measures and downgrades when a segmented run will not fit; `Bank()` has no
downgrade and laid its **full measured run** out from the lane's left edge. At 2.0× with
the drawer open, `log.severity`'s four chips finished **104 px past** the sheet's right
edge — clipped by the region, invisible and unclickable. This is the "controls escaping
the lane" case, and it broke SPEC §2.2's universal right-bound rule.

**Fixed** by scaling the run into the lane (D19.3). Pinned by a new `overlay_atoms` case;
mutation-checked — with the clamp disabled it fails with `801.08 <= 697.0`.

### 3.4 A chip bank could not be operated by keyboard at all — **high**

`AdjustValue()` explicitly refuses `Kind::Bank`, and a bank has no popup, so `log.sources`
and `log.severity` were **pointer-only** — in a shell whose spec devotes §8.2 to the
keyboard, and in a project that forbids synthetic pointer input and therefore could not
test them at all. Verified dead on `→ ← Space Enter` in the Sheet *and* in the Inspector,
in all three hosts.

**Fixed:** a chip cursor moved by `←/→` and toggled by `Space`/`Enter`, shared by both
hosts through one helper, drawn as SPEC §7.3's `Accent @ 85%` focus ring (D19.4, D19.5).
Verified afterwards in the Sheet, in the Inspector, and at 2.0× with the drawer open.

### 3.5 SPEC §2.4's affordance column was never painted — **medium**

`RowCtx::Affordance()` had had **no call sites since P1** (D18's own still-open list). The
column was allocated on every row of the product and drawn on none, so a sheet gave no
sign that a row owned Params or a Details page. With the Inspector hidden that chevron is
the *only* advertisement of depth the sheet has. **Fixed:** chevron and lock, by §2.4's
fixed priority, suppressed inside the Inspector (D19.6).

### 3.6 The sheet header was missing both of its specified chips — **medium**

SPEC §1.1's D9 amendment names exactly what the shipping header may carry: *"the
breadcrumb, the `differs N` chip, and the `inspector hidden` chip"*. Only the breadcrumb
and the layer badge were drawn. **Fixed;** `differs` is counted from the area's own
entries and their params, never typed.

### 3.7 A composite's value was the raw axis-A integer in two places — **low**

The palette showed `monitor.anchor = 0` and `monitor.color_fps = 8116985`; Details'
binding grid printed `NOW 0` two lines below a VALUES line reading `top-right · 32 / 32`.
**Fixed** — both now call `CompositeValue()`, which already existed.

### 3.8 `overlay_e2_select` was a second selection path — **low, but it invalidated evidence**

It assigned `s_sSelectedEntry`/`s_eMode` by hand instead of calling `Select()`, skipping
the Inspector focus reset and the explain-page close. Every keyboard test driven from the
console was therefore exercising a state the product never reaches. **Fixed** (D19.8).

---

## 4. Config safety, on disk

### 4.1 A pre-existing config loads intact and is not rewritten

A `global.json` was written by the branch, then hand-edited to add values a "before this
branch" file would have (`fps_display.font_size 23.5`, `gamescope.sharpness 7`,
`notifications.muted true`) **plus two keys the schema does not know**
(`future_key_from_a_later_version`, `overlay.some_unknown_overlay_key`), plus a
`games/424242.json` and a `profiles/My Preset.json`. All three were stamped with an mtime
of **2020-01-02 03:04:05**.

A session then opened the overlay, visited **all eleven areas**, walked five of them with
`↓ × 8`, pressed `Ctrl+/`, `Esc`, `Ctrl+I × 3` and `Tab × 3` in each, and opened the
palette — **without editing anything**.

```
after the session:
1577930645.0000000000 CFG/gamescope-ritz/profiles/My Preset.json
1577930645.0000000000 CFG/gamescope-ritz/games/424242.json
1577930645.0000000000 CFG/gamescope-ritz/global.json
95a11f65...  CFG/gamescope-ritz/profiles/My Preset.json
7c8e8470...  CFG/gamescope-ritz/games/424242.json
7d047e8f...  CFG/gamescope-ritz/global.json
```

**Every mtime and every sha256 is byte-identical to the seed.** Nothing is rewritten
implicitly, and the unknown keys are still there.

### 4.2 The values round-trip

Read back through the live registry after that load: `monitor.font_size = 23.5 px`,
`monitor.enabled = on`, `overlay.accent_hue = 300 deg`, `notifications.muted = on`,
`monitor.backdrop.opacity = 0.5`, `overlay.notification_placement = top right`,
`monitor.blend_mode = alpha`. Bool, int, float, string and enum-as-int all survive.

*One thing the reader should know and this pass did not change:* `display.sharpness` reads
`25 %` in that session rather than the seeded `7`, because the session was launched with
`--sharpness 5` and the **command line outranks the config layer** at runtime. That is
pre-existing routing, visible identically in the legacy panel, and not an E2 behaviour.

### 4.3 Nothing is deleted without an explicit confirmed press

`config.delete` is the one destructive action in the product. After the §3.2 fix: one
press arms and deletes nothing; `Esc` disarms; walking to another row disarms; two
consecutive presses delete, and only then. `config.override on` → `off` rewrites
`games/424242.json` (deactivating it, never deleting it — as documented) and leaves
`global.json` untouched. Saving a profile creates `profiles/<name>.json` and touches
nothing else.

---

## 5. Legacy mode and the HUD

**Legacy (`overlay_e2 0`)**: the dock, the Gamescope panel, its four tabs, its position
and its geometry are all intact after **five on/off flips with the overlay open the whole
time**. Diffing the dock's crop before and after the flips: **0 differing pixels**. The
panel's crop differs by 0.77%, which is the game frame moving behind its translucent
fill. No leaked E2 chrome, no ImGui id collision, and `grep -icE "assert|abort|SIGSEGV"`
over the session log returns **0**.

**The HUD is untouched.** It shares `FpsDisplay.cpp` with the settings panel and only the
settings half moved. Captured with the settings overlay closed, in legacy and in E2:

| pair | differing pixels in the HUD crop |
|---|---|
| legacy t=0 vs legacy t=1s (**the noise floor** — live digits and the frametime graph) | 10.96% |
| **legacy vs E2** | **9.02%** |
| E2 t=0 vs E2 t=1s | 7.73% |
| legacy vs legacy-after-E2 | 9.80% |

Cross-mode difference is **below** the same-mode noise floor. Side by side, the two crops
are identical in module set, order, typography, backdrop, rounding, spacing and colour;
only the live numbers differ.

---

## 6. The seventh "renders but does nothing" — **yes, and it is the biggest one**

> **CLOSED 2026-08-23 by D20.2 — built, not deleted.** `DrawSheetBody()` now lays out
> `nColumns` columns, each with its own lane, packing **whole groups** by the mockup's
> own greedy balance. `LayOutSheetColumns()` in `Layout.cpp` is the single place a
> column's geometry is decided, and D17's drawer occlusion became a per-column question
> that reduces to the old single subtraction at one column (pinned by a test).
> `Solve()` also gained `bUnsplittable` for escaped panels and content bodies, so the
> printed count and the drawn count are one number by construction. Verified live:
> 3 columns at 0.5×, 2 at 0.75×/1.0×, 1 at 1.25×, and `system.log` correctly reports
> `1 col` where `system.monitor` reports `3 col` at the same scale. Four new
> `overlay_shell` cases. **The section below is kept as the record of how it was found.**

**The multi-column sheet is computed, displayed, and drives nothing.**

`Solve()` computes `nColumns` with SPEC §8.3's content cap
(`min(widthAllows, ceil(rows/12))`). `shell.layout` prints it. `DrawSheetBody()` has one
`y` cursor, one `rcCol`, and draws **one column at every scale**. Measured live on the
25-row Monitor area:

```
scale 0.5   system.monitor : rail 232 · column 400 · sheet 1728 · 3 col · step -1
scale 0.75  system.monitor : ...                              · 2 col · step 0
scale 1.0   system.monitor : ...                              · 2 col · step 0
scale 1.75  system.monitor : ...                              · 2 col · step 2
```

The 0.5× and 0.75× captures show a single tall column with a scrollbar. SPEC §8.3 calls
two columns the ordinary case at 0.75× and 1.0×, and names the content cap as *"a one-line
rule that removes"* half-empty columns — a rule that currently removes nothing because
nothing reads it. `grep -n nColumns src/Overlay/UI/` returns three hits: the assignment,
the struct field, and the `printf`.

**Not fixed**, deliberately — D19.9. A balanced multi-column sheet touches the group
bands, the composite band's `n × 44`, the content body, selection, and the clipper; that
is a phase, not a test-pass repair, and guessing at it is precisely what D16.2 declined to
do with the drawer. The readout was also deliberately **not** patched to print `1`: the
honest number disagreeing with the screen is what made this findable.

---

## 7. Found and left, with reasons

### 7.1 The rail draws letters, not SPEC §8.0's drawn icon set — ~~left~~ **CLOSED**

> **CLOSED 2026-08-23 by D20.1.** The eleven glyphs are drawn from a `constexpr` table
> in `Icons.h` (imgui-free, so the geometry is unit-testable) through one
> `glyph::RailIcon()` that holds no coordinates. Screenshot-verified at 0.5× / 1.0× /
> 1.5× / 1.75× / 2.0×, expanded and collapsed: the three colliding pairs are now
> **two faders vs three solid bars**, **two offset cards vs one folded page**, and
> **a three-layer stack vs a framed window**. Two new `overlay_ui` cases pin that every
> area has a glyph, that no two are the same drawing, and that none escapes the
> 24-unit box; both mutation-checked. This was also the user's own critique point.


The rail shows the area title's first character. SPEC §8.0 specifies eleven inline-SVG
glyphs on one 24-unit grid, and the P2 comment in `DrawRail()` says P3 would draw them;
P3 did not. **This is not only cosmetic:** at ladder step ≥ 1 the rail collapses to icons
and the label disappears, and the letters then collide — `Mixer`/`Monitor` are both `M`,
`Profiles`/`Per-game` both `P`, `Shaders`/`Shell` both `S`. Three of eleven areas are
unidentifiable at 1.5× and above. Left because eleven drawn glyphs with their own
acceptance criteria (one silhouette at 12 px, no new detail at 48 px) is a self-contained
piece of work, exactly as that comment says. **Recommended for P5, ahead of the deletion**
— it is the last thing the icon rail is missing.

### 7.2 `Kind::Meter` has zero registrations — ~~left~~ **CLOSED**

> **CLOSED 2026-08-23 by D21.1 — registered, not deleted.** SPEC §3.8 does not only
> declare the kind, it names the instance, so the kind was built out rather than
> dropped: `display.budget_meter` in the frame limiter's Diagnostics group, reporting
> the share of one frame's budget the game actually spent. A **percentage**, because a
> Meter's range is fixed at registration and the budget is not. Verified live at
> `39 %`. Registering it immediately exposed a second defect — the palette printed a
> **blank** value, because `PaletteValueText()` fell through to the binding and a Meter
> has none. That is D19.7's composite bug a third time; fixed with one shared
> `MeterValue()` the sheet and the palette both call.

The kind is declared, `controls::Meter()` is implemented, `UsesValueColumn` and
`IsReadOnly` both handle it — and `grep -rn '\.Meter('` over `src/Overlay/` returns **0**.
SPEC §3.8 states *"`display.budget_meter` is the drawn instance — the first version
declared this kind and registered nothing that used it"*, which is still true. So Meter's
rendering has never been exercised in the product. Either register it or drop the kind;
either way it is a registry decision, not a shell fix.

### 7.3 Smaller things, recorded rather than fixed

- ~~**A Colour composite's `←/→` steps the packed `0xRRGGBB` integer by one.**~~
  **CLOSED 2026-08-23 by P5.** Refused, exactly as suggested. `Adjustable` grew the
  composite kind so the refusal is specific to `Color`: an Anchor's axes and a Hue's
  degrees are genuinely ordered and still adjust. One test covers both halves and
  asserts the binding is **unwritten** on refusal, not merely that the call returned
  false — a refusal that still moved the value would be worse than the bug.
  Mutation-checked.
- ~~**SPEC §6.3's inline param expansion does not exist.**~~ **CLOSED 2026-08-23 by
  D20.3 — built, and the comment corrected.** `DrawInlineParams()` renders a row's
  params beneath it in the Sheet's own Row grammar whenever the Inspector is hidden,
  through the *same* `DrawSharedControl()` the sheet row and the Inspector call — which
  is SPEC §6.3 clause 1's "one code path" holding literally rather than by assertion.
  The `›` becomes a `⌄` when open; `→`/`Space`/a click on the chevron expand, `↓` walks
  into the params, `←/→` adjust the focused one, and `Esc`'s "inline expansion" rung
  now has something behind it. Screenshot-verified end to end on
  `image.shaders.adaptive_brightness` (the Six Budget's live maximum): six params drawn
  inline, focus moved into them, `Target brightness` adjusted 0.5 → 0.508 with the
  Inspector hidden, then collapsed by `Esc` with the row still selected.
  The comment above `DrawExplainPage()` said *"params render inline (P3)"* while no such
  code existed; it now says what is true **and records that it used to lie**, so the
  correction is auditable rather than invisible.
- **The Overview card is a stub.** SPEC §5.5 asks for the differing-settings list,
  `WRITES TO`, `EFFECTIVE PATH` and three actions; the shell draws the crumb, the area
  summary and the `sheet N rows · inspector N params · 0 unreachable` line.
- **Details' binding grid is partial** — `now / default / range / options / kind / key`
  are drawn; §5.1's `writes / applies / parent` and the `related` jump links are not.
- **A Facts summary too wide for its zone clips its tail** (`128 lines · 2 er:`) rather
  than ellipsising. This is `DrawText`'s documented left-align fallback (D15) doing the
  right thing — losing the end is legible, losing the beginning is not — but an ellipsis
  would read better.
- **`controls::Text` uses `*` as its edit glyph** where SPEC §3.6 asks for `✎`. `*` is in
  the baked range and `✎` is not; the honest repair is a drawn glyph, like the chevron
  and the lock.
- ~~**The sheet's footer legend clips at 2.0×** (`Esc back` is cut).~~ **CLOSED
  2026-08-23 by P5.** The line now drops hints from the **left** through progressively
  shorter forms, chosen by measuring rather than by scale (the sheet's width depends on
  the Inspector's host and the drawer, not the ladder step alone), so `Esc back` is the
  last thing standing. Losing the tail was losing the one thing a user who cannot read
  the rest of the line actually needs.
- ~~**`widgets::Checkbox` still exists** with zero callers.~~ **CLOSED 2026-08-23 by
  P5**, together with eight of its nine neighbours in `Widgets.cpp` and with
  `Escape()` itself, as that entry predicted. Only `ApplyStyle()` survives, because
  the shell's own ImGui context still needs a styled baseline.
- ~~**The palette footer overlaps its last row at 2.0×.**~~ **CLOSED 2026-08-23 by
  P5.** `DrawPalette()` took nine rows unconditionally, summed the panel height from
  that, then clamped the finished panel to the slab — and the footer is drawn *from*
  the clamped edge while the rows were laid out against the unclamped height. The row
  count is now decided from the space available before the panel is sized, so there is
  nothing left to clamp.
- **The Overview card and Details' binding grid are still partial** — the two entries
  above them in this list. **Left consciously in P5** and the reason is worth writing
  down: they are *incomplete*, not *broken*, and the legacy path was never a fallback
  for them, because the legacy UI had no Overview and no Details page at all. Deleting
  it cannot make either worse. They are the largest remaining pieces of SPEC §5.1/§5.5
  and want a phase of their own.
- **A stray ImGui nav-cursor rectangle was seen once** on an unselected bank chip after a
  long key sequence, and could **not** be reproduced in a clean session. Recorded because
  D18(d) turned ImGui's nav off for E2's frames and this looked like a survivor.

---

## 8. The design's own rules — do they hold?

| Rule | Verdict |
|---|---|
| **Right-bound alignment is universal** (§2.2) | Held everywhere **except** the chip bank, which escaped the lane at 2.0× — §3.3, now fixed and pinned by a test. |
| **One control height** (§3.0) | Holds. `RowCtx::PlacePx()` is the only allocator and always returns a `kControlH`-tall rect; `overlay_atoms` asserts the *registered* hit box equals it for every atom at every scale. |
| **One row height, 44** (§1) | Holds. `LinesFor()` is the only answer, and a composite is `n × 44`. |
| **No checkboxes anywhere** (§3.1) | Holds in the E2 path — zero `Checkbox` call sites. The dead `widgets::Checkbox` declaration survives (§7.3). |
| **The Six Budget** (§5.2.3) | Enforced at registration with an abort; the message exists in `Registry.cpp` and `PARAMETERS n of 6` is drawn. The live maximum is 6 (`image.shaders.adaptive_brightness`), at the cap with zero headroom. |
| **The Prefix Law** (§5.2.2) | Enforced at registration, and re-checked per area on a dynamic rebuild. All 26 live param ids are `<parent>.<leaf>`. |
| **Required help** (§5.2.1) | Enforced; `SelfTest()` runs after `RegisterAll()` and again per rebuilt dynamic area. |
| **The Reachability Law** (§6.3) | Holds, **and its specified mechanism now exists** — D20.3 built the inline expansion, so the sheet alone shows and edits a param with the Inspector hidden. `Ctrl+/`'s full-sheet page and `Ctrl+K` remain the other two routes. |
| **The Inspector has no authoring API** (§5.2.0) | Holds. Nothing in the Inspector section takes a callback, a lambda or a string a panel typed. |
| **`differs` has one encoding** (D6) | Holds — the 2 px accent edge, plus the header chip this pass added. |
| **The affordance column holds at most one glyph** (§2.4) | Now true rather than vacuously true — §3.5. |
| **The rail counter means one thing** (§8.1) | **Not drawn at all.** No rail item carries a counter. Not a lie, but not built either; the header's `differs N` chip now answers the same question per area. |
| **Every hairline is `max(1, floor(scale))`** (§8.3) | Holds — `Hairline()` is the single implementation and every rule goes through `HLine`/`VLine`. |
| **The glyph range** (D18.4) | `overlay_e2_glyphs` over the live registry: **clean**, every string inside U+0020..U+00FF, in every session. |
| **Discoverability gate** (B's `WorstCharsToReach`) | **2** over the live registry. |

---

## 9. What this pass could **not** test

Stated plainly, because an untested claim in a report is worse than a gap.

1. **Anything requiring a pointer.** Clicks, drags, hovers, wheel, and the click-to-drag
   slider path are all unreachable — `ydotool` and synthetic pointer input are permanently
   banned (D4). Everything above was reached by keyboard or by a binding write. In
   particular: **the drag path of every slider, the anchor grid's click targets, the hue
   rail's click-to-pick, the Text field's click-to-edit, and every chip's mouse hit box
   are unverified by this pass.** The `overlay_atoms` suite covers their *registered
   rects*, which is the property that matters most, but not the behaviour.
2. **A real HDR display.** `display.hdr_facts` reports *"no HDR metadata reported"* on this
   machine, so the Facts row's populated state and its five `.Live()` readouts were only
   seen in their absent form (which is itself a specified state, §3.7, and it draws).
3. **A second connector.** `Output ▸ Second connector`'s `—` state is not registered in
   this build.
4. **`CompositeKind::Strip`.** No call site registers one; the band's body switch draws
   nothing for it, deliberately.
5. **Very long labels.** SPEC §1 caps a row label at 28 characters and calls the cap
   "lint-capped"; **no such lint exists in `Registry.cpp`** — the only length check there
   is the Six Budget. The longest registered label happens to be 26 characters
   (`Copy another game's config`) and the longest param title 19, so nothing currently
   exceeds the cap and the **label** column's truncation path was never exercised. The
   *value* column's was, and falls back to left-alignment as designed.
6. **Locale and IME.** The overlay is English-only by construction (Fonts.cpp bakes
   Latin-1) and the palette's query field is hand-rolled with no dead-key composition
   (D16.3).
7. **Multi-display / VR connectors.** One nested output only.
8. **Sustained soak.** Sessions ran for tens of seconds each, not hours; no memory or
   atlas-growth measurement was taken across repeated scale changes (the atlas rebuilds
   per effective scale, #38).

---

## 10. P5 addendum — the deletion pass (2026-08-23)

Added after this report was written, when P5 closed its open list and deleted the
legacy UI. Decisions: `../../AUTONOMOUS-DECISIONS.md` **D21**.

### 10.1 One defect this report missed entirely: the rail does not scroll

Not in §7 because it was never looked for. At 2.0× the rail's eleven items and three
section breaks are **taller than the rail**, and `DrawRail()` drew them from an absolute
y with no clip and no scroll — so the surplus was painted past the bottom edge and lost.
**Appearance and Shell were unreachable by pointer.** The command palette still found
them, which is exactly why §2.4's keyboard sweep scored every area as reachable and this
went unrecorded.

Pre-existing (it clipped letters before D20.1's icons landed), but P5 removes the
fallback, so it became the only behaviour. **Fixed** — the walk is now defined once and
used by both the measure and draw passes; the offset is `RailScroll()` in `Layout.cpp`,
imgui-free for the same reason `ConfigureRowsHeight()` is. Three `overlay_shell` cases,
mutation-checked.

*The lesson for the next report:* "reachable by keyboard" and "reachable by pointer" were
treated as one property here, and they are not. The palette can reach anything the
registry contains whether or not it is on screen.

### 10.2 Re-verified after the deletion

| Check | Result |
|---|---|
| All eleven areas selected at 1.0× and 2.0× | no errors |
| Ladder vs SPEC §8.3 | `rail 232 · column 400 · sheet 928 · step 0` and `rail 60 · drawer 400 · sheet 804 · step 2` — matches |
| `display.budget_meter` (the first live `Kind::Meter`) | reads `39 %` |
| Glyph range (`overlay_e2_glyphs`) | clean, every string U+0020..U+00FF |
| `assert|abort|SIGSEGV` over every session log | **0** |
| **§3.2's armed delete, re-verified** | arm → `Esc` → one more press: **file survived** |
| **§4.1 config safety, re-verified** | every sha256 and mtime byte-identical after a full passive walk; both unknown keys still present |
| HUD parity | **proved, not sampled** — `git diff` over `FpsDisplay.cpp` across the phase has zero added code lines |
| `meson test -C build` | **68/68**, `overlay_shell` 28 cases / 300 assertions |

### 10.3 What this pass could not do — one new item for §9

**No screenshots.** `grim -g` captures an **output region**, not a window, so a nested
gamescope that is not frontmost yields the host desktop instead — useless as evidence,
and a capture of the user's own screen. A first attempt did exactly that; its output was
destroyed unread and the pass was redone entirely through the overlay's own console
commands. Everything in 10.2 is a value read back out of the running compositor.

**So visual confirmation of the post-deletion result is still owed.** Nothing here proves
a pixel was drawn — only that the state behind it is right. That is a weaker claim than
§2.3's, and it is the one thing P5 leaves for the next pass to close.

