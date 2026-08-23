# Decisions taken without the user

The user went to sleep on **2026-08-23** having said: *"I will go to sleep now, so you will have
to make decisions. This is fine, since you are working on another branch. Write down the
decisions, that you made, so i can confirm that they are fine later!"*

Everything below was decided in their absence and **needs their confirmation**. Newest block on
top. Each entry records what was chosen, what the alternative was, and why — so disagreeing is
cheap.

---

## 2026-08-23

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

