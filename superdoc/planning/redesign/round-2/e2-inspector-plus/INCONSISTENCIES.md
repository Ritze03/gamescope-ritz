# E2 — inconsistency ledger

The redesign exists because of one complaint: *"Every module/window looks soooo different.
Nothing is really consistent."* This file is the audit that complaint deserves — what was
found in the E2 mockup, what was fixed, and what is left for the user to decide.

It is written to outlive the mockup: **this is the checklist the real implementation gets
audited against.**

---

## 2026-08-23 — first systematic audit

Method: every area compared against the busiest one; every CSS class checked for a second
class doing the same job; every number that appears twice checked for whether it means the
same thing; every rule stated in `SPEC.md` checked against the markup; every state that a
list, a value or a control can reach checked for whether it was ever drawn.

### A. Fixed — same concept, two renderings

| # | Found | Fixed as |
|---|---|---|
| A1 | **The rail counter meant five different things.** `Shaders` showed `3` (effects installed), `Mixer` `2` (streams), `Monitor` `4` (modules on), `Log` `2!` (errors), and every other area showed its differs-from-default count. One glyph slot, five meanings. | The counter means **exactly one thing: settings in this area that differ from default.** The single alternate form is a severity count, drawn in the danger colour with a `!`, reserved for a captured-error count. |
| A2 | **Every control had its own height.** Segmented / dropdown / stepper / text were 26; the switch was 30×15; the slider hit box was 22; the meter 5; the anchor grid cells 30. Nothing agreed. | One token, `--H: 28`, is **the** control height. Every control's *hit box* is `--H` tall. Box controls fill it; the switch (40×20), slider (8 track / 20 handle) and meter are shorter graphics centred in an `--H` box. The anchor grid's cells are on the same `--H` module. |
| A3 | **The switch was the smallest control on screen** — 30×15 with an 11px knob, next to 26-tall neighbours. It also contradicted shipping issue #23, which deliberately raised every control baseline 20–25%. | Same ~25% uplift applied to B's control geometry, snapped to the 4-unit grid: **switch 40×20, knob 16, travel 20**. The E-grammar demo keeps the old 30×15 switch so the difference is visible side by side. |
| A4 | **A choice looked like two different controls depending on where it was drawn.** In the sheet a `ch` could render segmented; in the inspector it was forced to a dropdown by a hardcoded `!inInsp`. | One helper, `segFits(entry, lane)`, measures for both hosts. A choice renders the same way anywhere it fits. |
| A5 | **Three names for one text-action.** `.txtact` (footer, group all/none), `.ilink` (inspector links) and `.explainpage .back` were three classes with the same look and job. | One class, `.txtact`, with a `.blk` block modifier. |
| A6 | **Three alphas for "the accent as a data bar."** Frametime HUD `.72`, sparkline `.55`, meter `.85`. | Tokenised; the page HUD uses `.bar-a` / `.bar-w`. |
| A7 | **`--acc-handle` / `--acc-icon` / the HUD's `#89E0F8` were literals outside the accent family**, so the hue slider could not move them. | Promoted to real family tokens (8 tokens, fixed L/C, only H moves). |
| A8 | **`.cb` (checkbox) was still in the stylesheet** although `SPEC.md` §3.1 says Checkbox is *deleted, not deprecated*. Dead CSS that contradicts a stated law is worse than dead CSS. | Removed. There is now no way to express a checkbox in this file. |
| A9 | **`.dot.ok` / `.warn` / `.dgr` were declared and never drawn.** | The title-bar dot now reports session health from the captured buffer (errors → danger, warnings → warn, otherwise ok). |
| A10 | **`.empty` was declared and never drawn**, so no list had an empty state. | Wired: an empty profile list, an over-filtered log, and a Facts row with nothing connected all draw it. |
| A11 | **The log filter bar was inline-styled** while every other component was a class. | `.filterbar .f` / `.fl`, and the bar is now **rendered from the registry** — `log.sources`, `log.severity`, `log.filter`, `log.autoscroll` are real settings, findable in `Ctrl+K`. |
| A12 | **Two disabled mechanisms.** `dis:'reason string'` and `dep:'other.id'` + `depmsg:` were two code paths for one state. | One: `dep(entry) → reason | null`. A param inherits its parent's reason unless it is the *cause* of it (`ni`), which was a real trap — turning Mute on used to disable the Mute switch. |

### B. Fixed — values that should derive but were hardcoded

| # | Found | Fixed as |
|---|---|---|
| B1 | **Accent literals everywhere.** `rgba(54,189,221, …)` appeared in ~30 rules; `applyHue()` set six tokens that most of the stylesheet ignored. This is the user's critique point 5. | Every accent-derived colour is `rgba(var(--accRGB), a)`. The only accent literals left in the file are the eight token declarations, which JS overwrites at boot. |
| B2 | **`46%` appeared three times** as `calc(46% - 12px)`, `calc(46% - 36px)` and `calc(46% + 12px)` — one rule, three expressions. | `--labz`. |
| B3 | **The sheet's 24px padding and the column gap were the same number typed twice.** | `--pad`. |
| B4 | **The affordance column's `28` was typed in four rules.** | `--aff`. |
| B5 | **Row height `44` appeared in the row rule, the band line, the band affordance and `n*44` in JS.** | `--row`, and bands are `calc(var(--row) * n)`. |
| B6 | **Gaps `9` (B's atom gap) and `3` (B's segmented gap) were invented per-control.** | `--gapA`, `--gapS`. |
| B7 | **`audio.volume` baked its unit into the value string** (`"−7.5 dB"`) while every other setting declared `u:`. The hue row rendered its value as `oklch(.74 .12 218)` — a debug string where every other row shows a number. | `u:'dB'` and `u:'°'`; `fmt()` never bakes a unit. |

### C. Fixed — rules `SPEC.md` states that the mockup broke

| # | Rule | Violation | Fixed as |
|---|---|---|---|
| C1 | §5.3: *"A parameter looks and behaves identically in the Inspector, in the inline fallback and in the Sheet."* | Inspector rows were **40** tall and sheet rows **44**. The spec contradicted itself in the same paragraph. | Inspector rows are `--row` (44). There is now exactly one row height in the product. |
| C2 | §1: a row shows *"what a setting is called, **what it is set to**, and how to change it."* | Switch, stepper and text rows showed **no value at all** — `showVal` covered only sliders, meters, facts and composites. The design's own thesis was unmet on its most common row type. | The **value-column rule**, stated once: the value column carries the resolved value for every control that *cannot show its own* (switch, slider, stepper, meter, facts, composite). Segmented, dropdown and text field show theirs inside the control and never duplicate it. |
| C3 | §3.9: *"at most two buttons per row."* | `profiles.actions` drew **three** (apply / rename / delete). | Two rows: *Selected profile* (apply · delete…) and *Save current settings* (save as profile), the second gated on the name field. |
| C4 | §3.9: `Danger().Confirm()` — *"a destructive button without a confirmation does not compile."* | No destructive button in the mockup confirmed anything. | Danger verbs **arm** on the first click (`confirm — delete`) and fire on the second; `Esc` disarms. |
| C5 | §3.6: *"Validation message sits in the Inspector, not under the box; the box border turns Danger."* | No text field validated anything. | `profiles.name` validates live (charset + duplicate name); the box turns Danger and the reason appears in the Inspector. |
| C6 | §3.8: Meter is one of the ten control kinds. | **No entry in the registry used it** — the kind existed only in the helper. | `display.budget_meter` (frame budget used), read-only, in Frame limiter ▸ Diagnostics. |
| C7 | §3.11: *"Disabled … plus a **mandatory** reason string."* | Only three entries were ever disabled, and one hardcoded its reason at the call site. | 14 entries carry `dep`; turning off *Show system monitor* disables ten rows and a composite, each with a reason. |
| C8 | §5.2 clause 2, the Prefix Law, and clause 3, the Six Budget. | Stated but never checked. | Asserted at load with `console.assert`, exactly where `Registry.h` would assert at registration. Verified: 26 params, 0 violations, largest owner has 6. |

### D. Fixed — areas that got less attention than others

| # | Found | Fixed as |
|---|---|---|
| D1 | **Read-only Facts rows were sometimes their own group and sometimes buried among settings.** Upscaling and Output put them last; Frame limiter, HDR and Mixer mixed them in with real settings. | **Rule: every read-only row lives in a final group named `Diagnostics`.** All eleven areas now follow it. |
| D2 | **Six facts rows, six naming patterns** — `display.path_facts`, `display.conn_facts`, `monitor.sampling`, `audio.facts`, `display.limiter_facts`, `monitor.graph_live`. | `<area>.<topic>_facts` everywhere. |
| D3 | **`system.log` had an empty registry** (`groups:[]`) — a whole rail item with nothing registered, nothing searchable, nothing selectable. | Four real settings plus a Diagnostics group. |
| D4 | **`display.output` had three settings; `system.monitor` had thirteen.** Output was visibly thinner than its neighbours. | Added `display.rotation` and a second-connector Facts row (which is also the *unavailable value* state). |
| D5 | **Group headers were inconsistent.** `Effects` had a count but no `all/none`; `Modules` had both; nothing else had either. Counts were also hardcoded strings (`'1 / 3'`) that went stale the moment you flipped a switch. | Counts derive (`cnt: () => …`), and any group with a count also gets `all` / `none`. |
| D6 | **Label casing was a coin-flip.** "Allow Tearing", "Force Grab Cursor", "Mura Compensation", "SDR Gamut Wideness", "Override Global Config" were Title Case; "Refresh rate", "Colour range", "Frame rate", "Font size" were Sentence case. Group names mixed both. | **Sentence case everywhere** (entries, groups, params), acronyms excepted — which is what `ui-mockup-precise-spec.md` §2 already specified for parameter labels. |
| D7 | **`applies` was six unparallel phrasings** ("needs mode set", "needs restart", "on apply", "on save", "next frame", "live"). | A six-value enum: `live · next frame · on mode set · on restart · on apply · on save`. |

### E. Fixed — states that were never drawn

| State | Where it is now reachable |
|---|---|
| Empty list | Delete all three profiles → the group draws an empty state and points at the name field. |
| Over-filtered list | Clear the log's source bank → *"no line matches the current filter"* + a clear action. |
| Single-item list | Delete two profiles; the dropdown holds one option. |
| Validation error | Type `bad/name` into the profile name. |
| Disabled composite | Turn off *Show system monitor* → `monitor.anchor` (a 3×44 band) goes disabled with a reason. |
| Value unavailable | **Output ▸ Second connector** — nothing connected, so it renders `—` and its Details block says so, rather than inventing a value. |
| Very long value | `audio.stream`'s options are 40+ characters; the dropdown ellipsizes and keeps its right edge on the lane. |
| Destructive action, armed | The delete verb's confirm state. |
| Empty search | `Ctrl+K` with a query matching nothing. |

### F. Left for the user — two defensible readings, so not decided silently

1. **Area id vs. config-key prefix.** Four of eleven areas have a rail id that does not match
   their settings' key prefix: `system.monitor` holds `monitor.*`, `setup.pergame` holds
   `config.*`, `setup.appearance` holds `overlay.*`, `setup.profiles` holds `profiles.*`.
   Either the config keys should be renamed to match the rail, or the docs should state
   plainly that **area ids are UI grouping only and carry no promise about key prefixes**.
   The Prefix Law applies to *params*, not areas, so nothing is broken — but the palette's
   path column shows both, and the mismatch is visible there.

2. **Three simultaneous encodings of "differs from default":** the 2px accent left edge, the
   accent-coloured value, and the reset dot in the affordance column. That is arguably one
   too many, and the reset dot *displaces* the depth chevron (§2.4's fixed priority), so a
   row that both differs and has depth stops advertising its depth. Options: drop the value
   recolour, or move the reset dot into the Inspector only.

3. **The chip bank as an eleventh control kind.** B has one (`.bank`); E2's taxonomy did not.
   It is used here for `log.sources` and `log.severity` under a stated rule — *a bank is one
   setting whose value is a set; N independent binaries are still N switch rows* — which
   keeps the no-checkbox law intact. If the user would rather not add a kind, both settings
   collapse to dropdowns.

4. **Sections holding a single area.** `IMAGE` and `AUDIO` each have one rail item, so the
   section header buys nothing at 1.0× and costs a line at 0.5×. Either fold them into
   `DISPLAY`/`SYSTEM` or accept the header as a growth slot.

5. **The dev-instrumentation chips in the sheet header** (`text lines`, `row heights`,
   `col`, `ladder`, `sheet 804b`) are mockup instruments sitting inside product chrome. They
   are the evidence for the density argument, so they stay in the mockup — but they must not
   survive into the shipping header.

### G. Known reconciliations between direction B and E2

Recorded here because they are design decisions, not slips:

- **B's `.mini` slider is a fixed 118px track.** E2 binds the slider to the full lane
  (`PlaceFull`). B's *paint* is kept (rounded 8 track, gradient fill, haloed 8×20 handle);
  the width follows E2.
- **B's inactive borders and tracks fail the contrast floor** (`8%` = 1.21:1, `16%` = 1.57:1,
  `18%` = 1.69:1). All are raised to the existing tokens `--lineCtl` (4.09:1) and
  `--trackOff` (3.07:1). Every other B colour is kept verbatim.
- **B's `.val.neutral` (44% white) is 4.09:1**, below the 4.5 floor for a 16px value. E2 keeps
  `--t92` for an at-default value and the accent for a differing one.
- **B's `.grid3` 3×3 proxy glyph has no home in E2**, because E2 draws the real 3×3 grid at
  full size in the composite band. Removed rather than shipped as dead CSS.
- **B's `.state` text ("on"/"off" beside the switch)** moves into E2's value column, which is
  where every other value that a control cannot show itself already lives.
