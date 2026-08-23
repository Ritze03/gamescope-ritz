# E2 mockup — exhaustive interaction test report

Date: 2026-08-23. Target: `index.html` (base commit `20304d7`). Method: Playwright MCP
driving a real Chromium instance against the file over a local HTTP server (not
`file://`, which the sandbox blocks) — real clicks, real key presses, real typed
characters, plus JS-state sweeps for combinatorial coverage `mouse-only clicking
could not reach in reasonable time (every area × every scale × every host). Both
approaches used the real `render()` pipeline; none of this stubs the app.

## Coverage counts

| What | Count |
|---|---|
| Areas (rail items) | 11 |
| Sheet rows (registry entries) | 60 |
| Inspector params (`.Param()`) | 26 |
| Control kinds exercised | 13 (`sw`, `sl`, `st`, `ch`→segmented, `ch`→dropdown, `tx`, `bank`, `act`, `meter`, `anchor`, `hue`, `strip`, `graph`) |
| Scale steps | 7 (0.5×, 0.75×, 1.0×, 1.25×, 1.5×, 1.75×, 2.0×) |
| Inspector hosts | 3 (column, drawer, hidden) |
| Full `render()` calls in the final regression sweep | 154 (7 scales × 2 forced hosts × 11 areas), zero throws |
| Rows + params rendered in both Configure and Details, checked for throw/empty/`undefined`/`NaN` | 60 rows × 2 modes + 26 params = 146, zero failures |
| Reachability sweep (`ui_lint --host=inline` equivalent, Inspector forced hidden) | 60/60 entries, 26/26 params received a `[data-row]` rect |
| Guided tours run end-to-end | 9/9, zero throws, zero console errors |
| Console errors across the entire session | 0 (two `favicon.ico` 404s only, not app errors) |

## What was exercised

- **Every control kind, via real interaction**: switch click (+ reset), slider click-to-position
  and `←/→` step and clamp at max, segmented click, dropdown open/select (auto-downgrade
  confirmed on `audio.stream`, 40+ char options), stepper `+`/`−` click and clamp at 0,
  text field click→type→validate→commit(Enter)/revert(Esc), bank chip toggle, anchor
  grid click, hue composite preset-swatch click, two-stage danger action (arm → confirm,
  and arm → Esc disarm), `Ctrl+D` reset, `Ctrl+K` palette open/search/adjust-in-place/jump.
- **State coherence after interaction**: row value, row `.diff` edge, reset-dot affordance,
  rail differs-count, and (for switches) the value-column `on`/`off` text were all checked
  together after each change — all agreed in every case tried.
- **Both Inspector modes, every row kind**, including composites (`monitor.anchor`),
  read-only kinds (`facts`, `meter`, `graph` — auto-open Details, `ro` Configure cell),
  and disabled rows (`display.mura`, and eleven rows + one composite when *Show system
  monitor* is off) — 146/146 renders non-empty, no thrown exceptions, no leaked
  `undefined`/`NaN` text anywhere.
- **The full scale ladder in all three hosts**: measured every row's and composite's
  rightmost DOM descendant against its own row/band bounding rect at all 7 steps —
  zero elements escaped their lane after fixes (see Alignment below).
- **The five named fixes from the brief**:
  1. Modes are `CONFIGURE`/`DETAILS` only — confirmed, no third mode anywhere in the DOM
     or state machine.
  2. `grep -n "thread" index.html` → zero matches.
  3. Rail icons are inline SVG on a shared 24-unit grid; screenshotted at 0.5× (full rail)
     and 2.0× (icon-only rail, `rail: 60`) — legible and distinct at both extremes.
  4. Hidden-Inspector spine (`.reopen`, "inspector ›") — verified it appears when hidden
     and restores the region via **both** a real click on the spine and a real `Ctrl+I`
     keypress, each starting from the opposite state.
  5. Right-bound alignment — see below.
  6. Controls share one height (`--H`) — verified via the CSS custom-property system, not
     just asserted; see below.
- **The hue sweep** — see below, no survivors.
- **States nobody draws**: very long label (injected, confirmed `text-overflow: ellipsis`,
  no lane overflow); empty profile list (delete all three → `.empty` band + jump-to-name
  action, confirmed live); single-item dropdown (one profile left → dropdown still opens,
  one option, no throw); empty log search (`Ctrl+K` with a non-matching query → the
  documented "nothing matches" state); over-filtered log (empty source bank → `.empty` +
  "clear the filter" action, confirmed it actually clears and restores 36/36 lines);
  invalid text input (`profiles.name` = `bad/name` style text → live `Danger` border +
  validation sentence in Configure, confirmed from **both** duplicate copies of the field);
  disabled composite (`monitor.anchor` under *Show system monitor* off) — confirmed a
  **real** Playwright click on the disabled grid is blocked by `pointer-events: none`
  (Playwright's own retry-and-timeout on the blocked click is the proof); value at min/max
  (sharpness slider clamped at 20, fps-limit stepper clamped at 0, both via real
  keyboard/click, not by reading the registry's declared bounds).

## Alignment

Measured, not assumed: for every area × 4 scale steps (0.5×/1.0×/1.5×/2.0×), collected
every `.val` and `.ctlz`/`.bodyz` right-edge `getBoundingClientRect().right` within each
`.cols > .col`, per column. **Zero outliers** — every value right edge and every control
right edge landed on the same x within its column, at every scale tested. The right-bound
law holds universally, including for the composite bands.

## Hue sweep

Swept representative accent-bearing elements (rail active icon, selected-row background,
slider fill + handle, switch on-state track/knob, segmented active cell, verb chip, slab
border, mode-strip active tab) between h=0° and h=180°, plus the CSS/JS source grep for
the literal `54,189` (the boot-time accent RGB) and the eight fixed hex tokens
(`#36BDDD` etc.) — **both appear nowhere below the `:root` token declarations they belong
to.** Every sampled element's computed `background-color`/`border-color`/`color` changed
between the two hues. **No survivors.** (One element, the title-bar health dot, correctly
did *not* move — it was showing the fixed-hue `Danger` state because the log buffer has
captured errors, which is deliberate per §7.5: `Danger` is excluded from the accent
family. Confirmed this is intentional, not a bug, by checking its class (`dot dgr`) and
the design rule it implements.)

## Defects found, ranked by severity

1. **(High) Duplicate `id="txin"` broke text editing from the Inspector.** Selecting a
   text-kind row with the Inspector open in `column` host rendered **two**
   `<input id="txin">` elements (Sheet's own row + Configure's own-control mirror).
   `document.getElementById('txin')` in `render()`'s auto-focus tail always resolved the
   first one. Reproduced with real keystrokes: click the Inspector's copy, type — the
   first character registers, then focus silently jumps to the Sheet's copy, breaking
   further typing from the user's actual point of interaction. **Fixed**: `id` → class
   `.txinput`; the three id-gated listeners updated; `render()`'s focus logic no longer
   steals focus from whichever copy already has it; the `input` handler now follows the
   caret back into the Inspector's copy after it gets rebuilt mid-keystroke; a new guard
   stops a caret-placement click on the input from being misread as a row-select click
   (which was forcing the disruptive full re-render in the first place).
2. **(Medium) The log's text filter did not filter live**, contradicting the guided
   tour's own claim. Typing only touched the Inspector + footer (a deliberate
   perf/focus-preserving shortcut) which never calls `logHTML()`. **Fixed**: split
   `logHTML()` into `logbodyHTML()` (reusable) and the filter-bar shell; the `input`
   handler now refreshes `#logbody` and the match-count chip inline, ordered to read the
   live (not yet reverted) filter value.
3. **(Medium) `log.buffer_facts` was unreachable from the Sheet under every host** — a
   Reachability Law violation. The Log area's raw rendering path bypasses the normal
   group/row loop entirely, so this fully-registered Diagnostics Facts row never got a
   `[data-row]` rect anywhere, at any scale, in any host. **Fixed**: the filter bar's
   existing match-count chip is now also `log.buffer_facts`'s clickable affordance.
4. **(Low) The anchor composite's Sheet value dropped the margins.** `SPEC.md`'s own
   worked example is `top-right · 32 / 32`; the mockup rendered `top-right` only, in both
   the Sheet band and the Inspector's own-control row. **Fixed** with a shared
   `resolvedStr()` helper so the two can't drift apart again.
5. **(Doc-only) Two stale examples in `SPEC.md` §4.4** — `oklch(.74 .12 218)` for the hue
   composite (the *pre*-B7-fix debug string; the mockup already correctly shows `218°`)
   and a meter-embedded audio-strip value the mockup never produced. Corrected in
   `SPEC.md`; the mockup's code was already right, only the doc had drifted.

## Judgement calls (recorded, not silently decided)

- **`SPEC.md` §4.4 claims five composites; the mockup registers four.** "Colour override"
  (L/C/H rails + swatch, for a per-game colour override setting) has no corresponding
  registry entry — building one is a new feature (new control painter, new setting, new
  wiring), not a hardening fix, so it was **left undone**. `SPEC.md` §4.4 now says so
  explicitly instead of overclaiming "five composites drawn."
- **The Inspector's stale-mirror asymmetry while typing was left alone.** While editing
  from the Inspector's copy of a text field, the Sheet's copy of the same field does not
  live-update (only the side the user is actively on refreshes per keystroke; the other
  side catches up on commit). This is the *pre-existing* design (the same asymmetry
  already existed for Sheet-initiated edits, just in the other direction) — restructuring
  the render pipeline to eliminate it entirely would be a bigger, riskier change than the
  brief's "harden, don't redesign" scope justifies for a cosmetic staleness window that
  only exists mid-keystroke.
- **F1–F5 in `INCONSISTENCIES.md`** (area-id/key-prefix mismatch, triple-encoded "differs"
  state, the chip-bank taxonomy addition, single-item sections, dev-instrumentation chips)
  were already recorded as open questions by the prior audit and were not re-litigated —
  none of them are defects a `render()`/interaction test can adjudicate; they're product
  decisions for the user.

## What could not be tested

- **Real mouse drag** on the slider/hue-rail/fader tracks was exercised via click-to-position
  (confirmed) and `←/→` stepping (confirmed) rather than a literal drag gesture — Playwright's
  `browser_drag` was available but click + keyboard covers the same code path
  (`dragMove()`'s pointer math is identical to the click handler's), and time budget went to
  breadth (60 rows × 7 scales × 3 hosts) over this one control's gesture variant.
- **Visual/pixel-level regression** (as opposed to layout-rect measurement) was spot-checked
  via two screenshots (0.5× full rail, 2.0× icon rail) rather than exhaustively across every
  scale × area combination.

## Fix commit

Fixes are in `index.html`; the two doc corrections are in `SPEC.md`; the full findings
ledger, in the same style as the first audit, is a new §H in `INCONSISTENCIES.md`.
