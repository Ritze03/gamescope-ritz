# Conformance audit — the built E2 shell against the approved mockup

**Date:** 2026-08-24
**Branch under audit:** `feature/overlay-e2` @ `1fcfbb6` (the P5 merge)
**Reference:** `index.html` in this folder — the interactive mockup the user chose and critiqued
into shape. Where `SPEC.md` and the mockup disagree, the mockup is the contract (task brief), but
in practice **every divergence below contradicts both**.

This is an audit. Nothing was fixed.

---

## How this was produced

**Mockup:** rendered in a headless Chromium at a viewport that makes its `#vp` stage exactly
1920×1080 — the same pixel canvas the compositor draws into — and screenshotted per tour. Row and
lane geometry was measured with `getBoundingClientRect()`, not by eye.

**Build:** `./build/src/gamescope -W 1920 -H 1080 -w 1280 -h 960 -S stretch --adaptive-sync
--immediate-flips --expose-wayland --filter fsr --sharpness 5 -f -- vkcube`, `DISABLE_LSFG=1`,
throwaway `XDG_CONFIG_HOME`, the whole session inside `scripts/with-gamescope-lock.sh --timeout
120`. Driven exclusively through `overlay_e2_select` / `overlay_e2_key` / `overlay_e2_host` /
`overlay_e2_palette` over `gamescopectl` — no `ydotool`, no pointer injection.

**Screenshots** are `grim -g` bounded to this run's own window rectangle, obtained from
`hyprctl clients -j` filtered to the launched PID's process subtree. The script **refuses to call
`grim` at all** if that lookup returns nothing, which is the guard P5 did not have.

Paired evidence is in `audit-shots/`: `mock-NN-*.png`, `build-NN-*.png`, and stacked
`pair-NN-*.png` (mockup on top, build below, same scale, same crop).

---

## Verdict in one paragraph

The **skeleton is right**: slab, three regions, 232 rail / 926 sheet / 400 inspector at 1.0×,
three rail sections, groups-and-rows, 44-tall rows in the ordinary case, segmented controls,
switches, sliders, the anchor composite, the hidden-inspector spine, the palette, the two-column
Monitor sheet. Someone who approved the mockup would recognise the room.

What they would not recognise is the **furnishing**. The Inspector — the region the whole
direction is named after — ships about a third of its specified content and none of its actions.
The shell chrome above and below the sheet (title bar, footer) is a different, plainer thing than
the mockup's. The rail has lost its differs counters, which is the mockup's single most
repeated visual signal. And the Log area is not a reinterpretation of the mockup's Log — it is a
different design.

**Counted:** 24 divergences below. **7** are fully explained by a recorded decision, **3** are
partly explained (a decision covers the change but not its visible extent), and **14 are
unexplained drift** — including all five of the top-five.

> **Since-audit fixes.** These counts are the audit's original finding and are left as
> written; rows resolved afterwards are struck through in place with the decision that
> closed them, so the table stays a record of what was found *and* what is still open.
> Resolved so far: **10** (ellipsis at the clip point — D30.4, 2026-08-24), leaving **13**
> unexplained.

---

## Divergence table

Severity: **S1** = the first thing they would point at · **S2** = they would notice within a
minute · **S3** = they would notice on a careful pass · **S4** = cosmetic / arguable.

| # | Sev | Where | Mockup | Build | Explained by |
|---|---|---|---|---|---|
| 1 | **S1** | Inspector · Overview | SPEC §5.5 and the mockup both draw six blocks: title, one-line summary, **"4 OF 7 SETTINGS DIFFER FROM DEFAULT" with each differing setting and its default**, **WRITES TO** (file + reason), **LIVE / EFFECTIVE PATH**, budget line, and **two action buttons** (`reset category`, `copy from another game`). | `DrawOverview()` (`Shell.cpp:2943`) draws **three**: title, summary, budget line. The differs list, WRITES TO, EFFECTIVE PATH and both buttons do not exist. The region is ~90% empty. | **Unexplained.** No decision mentions cutting them. This is the screen the user meets on opening the overlay. |
| 2 | **S1** | Log area | One dense toolbar line (source chips · severity chips · substring field · `err 2` `warn 3` `36/36` · TAIL · `copy`), then numbered, timestamped, severity-coloured log lines with per-line selection; Inspector has **LINE / FILTER** tabs, the selected line's TIME/SEVERITY/SOURCE/SUBSYSTEM, a **KNOWN PATTERN** paragraph and `copy line` / `filter to vulkan`. | The toolbar is exploded into **six full-height sheet rows** (Sources, Severity, Text filter, Auto-scroll, Buffer, Copy to clipboard) consuming ~360px before any log text. Body is a raw monospace dump — **no line numbers, no timestamps column, no severity colour, no line selection**. Inspector shows only `SYSTEM / Log` + `31 of 31 lines shown` + budget line. | **Unexplained.** D15 records migrating Log to `Kind::Composite`; it does not record replacing the Log's design. |
| 3 | **S1** | Rail | Every rail item carries a **differs counter** (`4`, `1`, `2`, `5`…), with the Log's severity variant drawn in Danger as `2!`. SPEC §8.1 spells this out: *"The counter on a rail item means exactly one thing."* | **No counters anywhere.** Eleven bare icon+label rows. | **Unexplained.** |
| 4 | **S1** | Slab title bar | `● GAMESCOPE-RITZ   app 1174180   games/1174180.json   ⌕ ▤ ✕` — SPEC §8.1 draws it literally. | `■ GAMESCOPE-RITZ` on the left; the word **`settings`** in dim grey on the right. No app chip, no config-file chip, no search / inspector / close controls. | **Unexplained.** |
| 5 | **S1** | Footer bar | Left: **`reset category`** action. Right: **boxed keycaps** — `^K search` · `^I inspector: column` · `^D reset row` · `? details` · `↑↓ row` · `⌫ value`. The `^I` hint names the *current* host. | Left: nothing. One row of unboxed plain text: `^K search  ^I inspector  ^/ explain  Tab region  Esc back`. No `^D reset row`, no `? details`, no `↑↓ row`, no `⌫ value`, no host name, no keycap chrome. | **Unexplained.** `Ctrl+D` is in SPEC §8.2 and D14.8 implemented reset — the footer just never advertises it. |
| 6 | **S1** | Inspector · CONFIGURE | Header line is `SHARPNESS` in caps + kind chip `SLIDER` + state chip `DIFFERS`. `VALUE` section, then a **full-width Row-grammar block**: label, right-aligned value + differs dot, **slider on its own full-width line**. `PARAMETERS 3 OF 6` in the **same 44-tall Row grammar** (toggle, segmented, full-width slider). Closes with the Prefix-Law note and **three buttons**: `reset to default`, `copy key`, `details ›`. | Header is `Sharpness`, mixed case, **no kind chip, no differs chip**. `VALUES` + a small **`reset` text link**. The value row squeezes label, value and a **~130px stub slider onto one line** — a third of the mockup's track. Params (Monitor) use the same stub sliders where the mockup uses steppers. **No buttons at all.** | **Partly** — D6 moved reset into the Inspector and D14.8 implemented it, which explains the `reset` link existing; nothing explains it being a text link *instead of* the three buttons, nor the stub slider. |
| 7 | **S1** | Inspector · DETAILS | `BINDING` (NOW · DEFAULT · RANGE `0 – 20 · step 1` · KIND · KEY · **WRITES** · **APPLIES**), a **`KEYBOARD`** section, **`LIVE [READ-ONLY]`** with derived readouts, **`LOG LINES MENTIONING THIS KEY`**, and the closing "Details accepts no binding" note. | Five rows: NOW · DEFAULT · RANGE · KIND · KEY. No WRITES, no APPLIES, no KEYBOARD, no LIVE, no LOG LINES, no note. | **Unexplained.** |
| 8 | **S2** | Row height | **Every** row in the mockup is exactly 44 base; the Placement composite is exactly `3 × 44`. The user asked for one consistent control height. | Ordinary rows are 44, but Monitor's four **module-colour rows are ~88** (three stacked gradient bars + swatch), the **Frametime row ~100**, and Appearance's **Accent colour row ~88** (hue strip + swatch bank). Four distinct row heights on one sheet. | **Unexplained.** |
| 9 | **S2** | Monitor sheet | Left column: MONITOR · PLACEMENT (composite, Font size, Backdrop) · DIAGNOSTICS (Sampling, **live Frametime bar graph**). Right column: `MODULES 4 / 7` with **`ALL` `NONE`** group actions + seven toggles. **No colour rows exist.** | Adds a whole **`MODULE COLOURS`** group — four rows, each with a hex readout, a swatch and **three stacked gradient bars with round handles**. Easily the noisiest object in the product. `MODULES` header shows `7 / 7` but **lost `ALL`/`NONE`**. Frametime renders an **empty dark box with a padlock**, not a graph. | **Unexplained.** D14/D15 record the Monitor migration; neither records inventing a colour-picker control kind or dropping the group actions. |
| 10 | **S2** | Value column | The mockup truncates long values with an **ellipsis** (`Placem… top-right…`) — deliberate, shown in tour 4. | ~~Hard clipping, mid-glyph, no ellipsis: `top-right · 3:`, `240-frame windo`, `no HDR metadata :` (palette), `32` (Inspector). Reads as a rendering bug, not a design.~~ **FIXED 2026-08-24 (D30.4).** `Controls.cpp`'s `DrawText()` — the shell's single clip point — truncates to the last glyph that fits and appends `...`. Now reads `top-right ...`, `500 ms · 240-frame ...`, `31 lines · ...`. Three ASCII periods, not U+2026: `Fonts.cpp` bakes Basic Latin + Latin-1 only and the Geist faces carry no ellipsis glyph (same constraint as D18's drawn chevron). | **Resolved.** |
| 11 | **S2** | Focus | Region focus is shown by the accent edge and the selected row; there is no region-sized outline anywhere in the mockup. | `Tab` draws a **bright 2px blue rectangle around the entire Sheet region** — the largest single accent shape in the product. See `audit-shots/build-10-region-focus-ring.png`. | **Unexplained.** |
| 12 | **S2** | Sheet header | `DISPLAY / **UPSCALING**` — leaf in bold uppercase, then an **accent-tinted `differs 4` chip immediately after the crumb**. | `DISPLAY / Upscaling` — leaf mixed-case at the same weight as the parent, so the crumb has no focal point. `differs 4` is demoted to **dim grey text at the far right**, beside the layer badge. | **Partly** — D14.7 explains the layer badge (`global`, `app <id>`, `global only`) being right-aligned. Nothing explains the differs chip moving there or losing its accent, nor the leaf losing its emphasis. |
| 13 | **S2** | Rail composition | Eleven items: Upscaling, **Output**, Frame limiter, HDR, Shaders / Mixer, Monitor, Log / Profiles, Per-game, Appearance. | Eleven items — but `Output` is gone and **`Shell`** has taken its slot. | **Split.** `Output`'s absence is **D13.2** (no config keys exist for it) — accepted. `Shell` appearing as a product rail item is **unexplained**: D12 introduced `setup.shell` as P2 scaffolding, so that "there would be no selection anywhere in the product" while every real area was still escaped. Every real area landed in P3/P4; the scaffold was never removed. |
| 14 | **S2** | Palette | Matched substring **highlighted in accent** inside the description. `differs` chips alongside `param` chips. Footer uses **boxed keycaps** (`↑↓ move`, `←→ adjust in place`, `⏎ jump & select`, `esc dismiss`) and closes with the tagline *"params are searchable — depth is never hidden"*. | No match highlighting — matched and unmatched text identical. **No `differs` chips.** Footer is unboxed plain text; **no tagline**. Ranking and `←→` in-place adjust are correct. | **Partly** — D6 removed the *value recolour on the sheet*; it says nothing about the palette's `differs` chip, which is a separate signal. |
| 15 | **S3** | Sharpness semantics | Value reads `5`, range `0 – 20 · step 1` (`DETAILS`), matching gamescope's own sharpness scale. | Value reads `25 %`, range `0 .. 100`, default 10. A user who set `--sharpness 5` sees `25 %`. | **Unexplained.** |
| 16 | **S3** | Presentation group | Three rows: Allow tearing, Force grab cursor, **Mura compensation**. | Two rows — `Mura compensation` absent. | **Unexplained** in `AUTONOMOUS-DECISIONS.md`, though D13.2's reasoning (no config key, no setter) would cover it if stated. |
| 17 | **S3** | Scaler segmented | `auto · fit · fill · stretch · integer` | `auto · integer · fit · fill · stretch` | **Unexplained.** |
| 18 | **S3** | Profiles | `SAVED PROFILES` group is always present: `Profile` dropdown + `apply` / `delete…` buttons, reaching their **empty state together** with the list (tour 9's whole point). | With zero profiles the group **is not drawn at all**; the sheet opens on `NEW PROFILE`. There is no empty state, only an absence. | **Unexplained.** |
| 19 | **S3** | `Ctrl+/` explain page | SPEC §8.2: *"Configure + Details for the selected row (**full-sheet page when the Inspector is hidden**)"*. In column mode the Inspector already is the answer. | The full-sheet page opens **in every host**, so with the Inspector in column mode the same two bodies are drawn twice, side by side. | **Explained in code**, not in the decisions file — `Shell.cpp:4034` argues "strictly more readable rather than a different answer". Diverges from SPEC's stated condition. |
| 20 | **S3** | Row lane | Label+value lane 375 base, control lane 452 (measured on the mockup at slab 1560). | Label+value ≈ 393, control ≈ 435. Sliders and segmented banks are ~18px shorter than the mockup draws them, on every row. | **Explained — D11.2.** The literal reading of the clamp is defensible; this is its visible cost, which D11.2 did not quantify. |
| 21 | **S4** | Sections | Five sections in early drafts. | Three: `DISPLAY`, `SYSTEM`, `SETUP`. | **Explained — D8.** Matches the mockup as shipped. |
| 22 | **S4** | Sheet header dev chips | `7 text lines · 1 row height · 1 col · ladder 0 · sheet 928b`. | Absent. | **Explained — D9.** Correctly not implemented. |
| 23 | **S4** | Two-column sheet | Monitor is two columns in the mockup too (tour 4). | Two columns, per `columns = min(widthAllows, ceil(rows/12))`. | **Explained — SPEC §8.3.** Conformant. |
| 24 | **S4** | FPS HUD | Drawn top-right in every tour. | Absent — the system monitor is off by default in a throwaway config. | **Not a divergence.** State, not design. |

---

## What is right, and worth saying

Not everything drifted, and the parts that hold are the expensive parts:

- **Layout geometry.** Slab, rail 232, inspector 400, sheet 926, 44-tall rows, 12-unit gutters,
  28-wide affordance column — all within a few pixels of the mockup.
- **The control atoms' identity.** Segmented banks, switches, sliders, steppers, chip banks and
  the 3×3 anchor grid all read as the mockup's versions at a glance.
- **The composite band.** Placement is a real composite, right-bound to the same control line as
  every switch, with its offsets demoted to Params — exactly the tour-4 fix.
- **The hidden-inspector spine.** Present, named, holding its own 20 units, restoring on click.
- **The palette's substance.** Ranking, `param` chips, `←→` adjust-in-place and `⏎` jump-and-select
  all behave as specified; only its *presentation* is thinner.
- **Group accent edges** for differing rows (D6's chosen single encoding) are drawn correctly.
- **Glyph hygiene.** `overlay_e2_glyphs` reports the registry clean — no fallback boxes.

---

## The shape of the problem

The unexplained divergences are not scattered. Sorted by where they live:

| Region | Unexplained divergences | Character |
|---|---|---|
| **Inspector** | 1, 6, 7 | Content and actions specified but never written. The three biggest items in the audit. |
| **Shell chrome** (title bar, footer, breadcrumb) | 4, 5, 12 | Placeholder chrome that was never replaced with the designed chrome. |
| **Rail** | 3, 13 | One missing signal, one leftover scaffold. |
| **Log** | 2 | A different design, not a thinner one. |
| **Monitor / Appearance** | 8, 9, 10 | Invented control kinds that break the uniform row height and clip their values. |
| **Polish** | 11, 14, 15, 16, 17, 18 | Individually small; collectively they are why it "doesn't behave like it". |

Four of the six rows above are **omission** — the designed thing was never built and nothing was
built in its place. Only the Monitor/Appearance row is **invention**, and only the Log row is
**substitution**.

That distinction matters for what to do next, and it is the one thing the phase reports could not
tell anyone: every phase verified that what it built worked, and none compared what it built to
what was drawn.
