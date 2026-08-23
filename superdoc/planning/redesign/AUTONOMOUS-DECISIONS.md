# Decisions taken without the user

The user went to sleep on **2026-08-23** having said: *"I will go to sleep now, so you will have
to make decisions. This is fine, since you are working on another branch. Write down the
decisions, that you made, so i can confirm that they are fine later!"*

Everything below was decided in their absence and **needs their confirmation**. Newest block on
top. Each entry records what was chosen, what the alternative was, and why — so disagreeing is
cheap.

---

## 2026-08-23

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

