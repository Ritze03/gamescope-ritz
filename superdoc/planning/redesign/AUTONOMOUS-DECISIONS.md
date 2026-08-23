# Decisions taken without the user

The user went to sleep on **2026-08-23** having said: *"I will go to sleep now, so you will have
to make decisions. This is fine, since you are working on another branch. Write down the
decisions, that you made, so i can confirm that they are fine later!"*

Everything below was decided in their absence and **needs their confirmation**. Newest block on
top. Each entry records what was chosen, what the alternative was, and why — so disagreeing is
cheap.

---

## 2026-08-23

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

