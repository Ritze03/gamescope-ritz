# Configuration-UI research — making the settings overlay more intuitive

Date: 2026-08-22
Status: **research and proposal only. Nothing here is approved or implemented.**

The ask, verbatim: *"Research best practices for configuration UIs, so it is more
intuitive, than it is right now."* The target is **intuitiveness**, not visual
polish — visual work is tracked separately (issues #23, #24, #32, #34, #37 and
`ui-mockup-precise-spec.md`'s gap list). The question this file tries to answer
is narrower and harder: can somebody who opens this overlay for the first time
**find** what they want, **understand** what it does, and **predict** what will
happen when they touch it?

Method: read the shipped panels (`src/Overlay/PanelDisplay.cpp`,
`PanelShaders.cpp`, `PanelAudio.cpp`, `PanelConfig.cpp`, `FpsDisplay.cpp`,
`Chrome.cpp`, `Notifications.cpp`), the intended structure (`SPEC.md`,
`ui-mockup-precise-spec.md`, `ui-design-guide.md`), all 25 recorded decisions
(`DECISIONS.md`), and every open issue (#19, #23–#37) so recommendations land
against where the UI is *going*, not only where it is. Then check that reading
against published interface guidance and against how comparable software solves
the same problems. Sources with dates are at the bottom.

---

## 1. What already works — the honest part

This is not a UI with a diffuse "needs UX love" problem. Several things here
are better than most shipped config UIs, and a few are better than the recorded
decisions that produced them. Naming them matters, because three of the ten
recommendations below are "do more of what this codebase already does well."

- **Everything applies live, with no OK/Apply/Cancel anywhere.** This is exactly
  what the GNOME HIG has recommended since HIG 1.0 — "instant apply", with an
  Apply button reserved for changes that take more than about a second. The one
  case where that exception would have bitten (switching ReShade effects forces
  a synchronous FX parse + SPIR-V compile + pipeline build on the vblank thread)
  was engineered away rather than papered over with an Apply button: decision
  #13's single combined `.fx` gated by per-effect uniforms means toggling an
  effect costs a uniform write. The guideline's exception was removed instead of
  obeyed. That is the better outcome.
- **Unavailable states are explained, not silently dead.** `PanelAudio.cpp` is
  the strongest example in the codebase: `wpctl` missing, no streams on the
  system at all, streams that exist but did not match this game, a manual pick
  that is not currently streaming — four *distinct* messages, each naming the
  real cause, plus a manual picker that is always available rather than only
  appearing as an error fallback. `PanelShaders.cpp`'s HDR gate names the
  limitation and says it is deliberate. `PanelDisplay.cpp` surfaces the
  Steam-focus override ("filter/scaler are temporarily forced to Fit/Linear")
  instead of showing controls that have quietly stopped applying. That last one
  is a genuinely unusual thing to bother doing.
- **The single auto-corrected sharpness slider** (decision #11) is the right
  call and was verified against rendered output, not shader math. Upstream
  `ValveSoftware/gamescope#515` asked for exactly this and was closed as not
  planned; the fork fixed it in the UI layer. "Higher always means sharper"
  is Nielsen's heuristic #4 (consistency and standards) done properly.
- **Honest degradation when no app id resolves.** `DrawPerGameTab()` says "No
  game identified for this session… All changes made in the other panels are
  going to global.json" rather than offering a per-game editor with nothing
  behind it.
- **The startup announcement is the right onboarding.** `SettingsOverlay.cpp`
  already draws "GAMESCOPE-RITZ ACTIVE / CTRL + SHIFT + O / opens the settings
  overlay" on launch. Discoverability of the entry point — which the brief
  flagged as a worry — is already solved, and solved in the only way that suits
  a game overlay: a non-modal, self-dismissing toast, not a tour.
- **Default panel state is one panel open (Display), not five.** Opening to a
  wall of windows would have been the easy mistake.

So the problems below are not "this UI is bad". They are, almost entirely, one
problem repeated: **the overlay knows things it does not tell you.**

---

## 2. Which published guidance actually transfers — and which does not

This is the part worth arguing, because a generic heuristics checklist would be
worthless here. A settings surface opened *mid-session, with the game paused
behind it* is a different artifact from a desktop preferences window, and about
a third of the standard advice is actively wrong for it.

### Transfers, strongly

- **Nielsen heuristic #1, visibility of system status** (Nielsen 1994, last
  updated 2024-01-30). This is the whole ballgame here. Three config layers, a
  full-snapshot override, a one-time profile copy, per-panel routing rules, and
  a fourth rule for `overlay.*` that ignores all of the above — and the UI shows
  one of those states, in one tab. Recommendations 1, 5, 6, 8 and 10 are all
  this heuristic.
- **Nielsen #3, user control and freedom** ("a clearly marked emergency exit").
  In a live-apply UI with no Cancel, the emergency exit is **reset**, and there
  is currently none anywhere in the overlay. Recommendation 3.
- **Nielsen #6, recognition rather than recall.** Five unlabelled dock icons,
  and controls named `NIS`, `RCAS`-derived "Sharpness", "protect skin tones",
  "min gain", "additive". Recommendation 2.
- **Nielsen #2, match between system and the real world.** "SHADERS" is the name
  of the implementation, not of what the panel does. Recommendation 9 — with a
  deliberate exception argued there.
- **GNOME HIG instant-apply.** Already followed; keep following it. It is the
  reason "reset" and not "cancel" is the right investment.
- **Material Design's grouping rule** — related settings under one subscreen,
  and the label that opens a group matches the subscreen's title. Issue #25's
  tabbed GAMESCOPE panel is this, and it is the right direction.

### Transfers only partly, or with a twist

- **Progressive disclosure** (Nielsen, 2006-12-03): show only frequently-needed
  features up front; everything else behind a second level. Valve implements
  precisely this on the Deck — per-game profiles and the deep knobs live behind
  **Quick Access Menu → Performance → Advanced View**. It demonstrably works on
  a handheld under time pressure. *But* the split costs a click every single
  session, and it is paid by exactly the user who went out of their way to run a
  gamescope fork. My call: **do not add an "Advanced" tier now.** Use tabs
  (#25's direction) instead — the same one click, but every option's *label*
  stays visible on the tab strip, so nothing is hidden, only deferred. Revisit
  if the SYSTEM MONITOR panel (#27–#29) really does land 30+ controls.
- **Material Design's secondary-text rule**: "the secondary text conveys the
  current *state* of the setting. Avoid describing the setting." Correct on
  Android, where labels are self-evident ("Wi-Fi", "Bluetooth"). **It does not
  transfer here**, because the labels genuinely are not self-evident — nobody
  knows what "protect skin tones" or "min gain 0.72" means from the label. This
  UI needs descriptive help text. The nuance Material is right about is *where*
  it goes: the panels already lean toward permanent grey prose under controls
  (`PanelDisplay.cpp`, `PanelShaders.cpp`, `PanelConfig.cpp` each have several),
  and more of that turns a panel into a wall of text. Hence recommendation 2 —
  descriptions move to hover tooltips; permanent inline text is reserved for
  *state* ("Steam window is focused…", "this game is snapshotted"), which is
  exactly Material's rule applied to the right channel.
- **Apple HIG's settings philosophy** — "successful apps work well for most
  people right away"; put frequently-changed options on the main screen, rarely
  changed ones progressively further away, and the rarest in the system Settings
  app. The *tiering* transfers. The *conclusion* inverts: this overlay exists
  precisely because these settings need changing while the game runs, and there
  is no "system Settings app" to banish anything to — the alternative to a
  control being here is a command-line flag the user cannot reach mid-session
  (`--force-grab-cursor` today, per issue #25). "Infer what you can from the
  system" does transfer and is already done well (audio stream detection, app id
  resolution).

### Does not transfer — do not do these

- **A settings search box** (VS Code, and Steam's own client settings). Search is
  the right answer to *hundreds* of settings. This overlay has roughly fifty,
  across five panels, and no guarantee of a keyboard — typing a query with a
  gamepad while a game is paused is worse than reading five dock icons. Skip it.
  Revisit only if the panel count roughly doubles.
- **"Confirm before discarding unapplied changes"** (Giguere's game-settings
  checklist, 2018-11-12, and every OK/Cancel dialog ever). There are no
  unapplied changes — everything is live. Spending that budget on *reset* is
  the correct live-UI translation of the same user need.
- **Onboarding tours / first-run walkthroughs.** The overlay is opened
  mid-session, sometimes mid-fight. Anything modal that must be dismissed before
  the user can reach the volume slider is a net negative. The existing startup
  toast is the correct size of onboarding, and it is already built.
- **NN/g's customization-features guidance** ("don't offer users features to
  customize the basic look… focus resources on the best design for the greatest
  number of users"). Written about mass-audience website homepages. This is a
  single-user personal tool where theming is a requested feature (decision #10,
  issue #37). Ignore it here.
- **MangoHud as a UX model.** Worth naming as an anti-pattern rather than a
  precedent: its configuration is a text file (`~/.config/MangoHud/MangoHud.conf`),
  and the community's answer has been third-party GUIs (GOverlay, MangoJuice)
  whose own recurring complaint is that the GUI and the effective config drift
  apart. This overlay's in-process, live, single-source-of-truth model is
  already the better design. The one thing worth stealing from MangoHud is that
  its per-parameter documentation is exhaustive — which is recommendation 2.

### The one principle that is specific to *this* artifact

Decision #4: **the overlay takes all input while it is open; the game gets none.**
That single fact changes the cost function. Being confused in a desktop
preferences window costs you a few seconds. Being confused here costs you the
game — you are stopped, possibly in a bad place, while you work out which of two
"Sharpness" controls you wanted. So: legibility beats density; recoverability
beats confirmation; nothing should ever be modal; and a control that cannot be
understood at a glance should be understandable at a *hover*, not a
documentation lookup.

---

## 3. Ten recommendations, ranked

Ranked by intuitiveness gained per unit of work, with prerequisites respected.
Sizes use the repo's own XS/S/M/L convention. "Conflicts" is checked against
every entry in `DECISIONS.md`.

---

### 1. Put the config layer in every panel's title bar

**What.** `Chrome.cpp`'s `DrawTitleBar()` draws a Mono 10.5 @30% meta slot right
of the title — and hardcodes it to the string `"gamescope-ritz"` on all five
panels (`Chrome.cpp`, `kMeta`). Five windows each repeating the application's
own name is the least informative use that slot could have. Replace it with a
live scope badge that names **which file this panel's edits land in**:

| Panel state | Badge |
|---|---|
| No app id resolved | `global` |
| App id, override off | `global` |
| App id, override on | `app 3746030` |
| Config → General tab, Notifications tab | `global only` |

**Why.** This is the single biggest legibility gap in the overlay. There are
*four* routing rules today and the UI states one of them, in one tab, in prose:
(a) Display/Shaders/Audio/FPS route through `config::EnqueueRoutedWrite()` — per-game
file if `IsSessionOverrideActive()`, else global; (b) `overlay.*` (the whole
General tab) always writes `global.json` via `EnqueueGlobalWrite()`, *even while
an override is active*; (c) notification placement is global-only (decision #25)
while notification muting is per-layer, in the same tab; (d) window geometry, once
issue #35 lands, will be a third global-only class. A user who enables "Override
Global Config" and then changes notification placement expecting it to be
per-game has been silently misled, and nothing on screen contradicts them.
VS Code solved the identical problem for user/workspace/folder settings with a
persistent per-setting scope indicator ("Modified in: Workspace"), and it is the
reason that editor's three-layer model is usable at all.

**Cost.** XS–S. The slot, the font, the colour and the draw call all exist.
`BeginPanelWindow()` gains a badge argument (or computes it from
`config::IsSessionOverrideActive()` / `SessionAppId()` directly). No schema
change, no persistence change, no new widget.

**Conflicts.** None. It *serves* decisions #19 and #25 — #19's own recorded
consequence says "any UI surfacing config state should make 'this game is
snapshotted and frozen' visible", which today happens only inside one tab.

---

### 2. A one-line hover tooltip on every control

**What.** Author a short description for each control and show it on hover, using
the tooltip already specified in `ui-mockup-precise-spec.md` §9 and already
implemented (`Chrome.cpp`'s `DrawDockButton()` calls `ImGui::SetTooltip()`; it is
the only tooltip in the entire overlay). Roughly fifty strings. Extend the same
pass to the dock buttons so they describe *contents*, not just repeat the window
title — `"Gamescope — upscaling, HDR, VRR, frame limit"` rather than `"Display"`.

**Why.** Nielsen #6. Today the only way to learn what "NIS", "protect skin
tones", "Min gain", "blend mode: additive", "Scaler: fit vs fill vs stretch" or
"Allow tearing" do is to try them and look. Several are not even reversible by
eye (min/max gain interact over seconds). ReShade — the closest possible
comparable, and the very engine this overlay drives — solved exactly this with
the `ui_tooltip` uniform annotation, added in ReShade 4.0, alongside `ui_label`
and `ui_category`; every serious community shader ships descriptions. This
overlay's `gamescope-ritz.fx` already declares each uniform with a `source`
annotation, so the parallel is direct even if the strings live on the C++ side.

Two details that make this good instead of noisy: (i) tooltips *replace* the
permanent grey descriptive lines that currently sit under controls, they do not
add to them (this is Material's secondary-text rule applied correctly — permanent
inline text is reserved for state, tooltips carry description); (ii) tooltips
must never be the *only* place a disabling reason appears — see recommendation 6.

**Cost.** M, but entirely in copywriting, not architecture. Zero risk. Can land
incrementally, one panel at a time.

**Conflicts.** None. §9's tooltip is a measured, specified component that is
currently used for one thing.

---

### 3. Reset affordances — the missing emergency exit

**What.** Add a `reset` link at group level (each `BeginGroupBlock()` section) and
a panel-level "reset this panel to defaults". Nine controls in `PanelShaders.cpp`
alone have no path back to their defaults short of the user remembering the
number or deleting a JSON file.

**Why.** With live apply and no Cancel, reset *is* the undo (Nielsen #3). It also
changes behaviour, not just recovery: people explore a settings panel far more
freely when exploration is cheap to undo, which is the difference between a user
discovering Vibrancy and a user never touching it. Both comparables do this:
Valve's Deck lets you "reset to system default settings at any time" right next
to the per-game profile toggle; Giguere's game-settings checklist calls out easy
reset explicitly. **And this design language already anticipated it**:
`ui-mockup-precise-spec.md` §1 defines the token `accent-link-dim` (`#6BCDE9`) for
exactly "footer action text (*reset*, *save as preset*, *live*)" — a reset
affordance is already specified in the measured design and simply was never
built. This is closing a spec gap, not inventing a control.

**Cost.** S–M. One small link widget in the existing language, plus a
defaults-source. `ConfigSchema.h`'s default member initialisers are already the
authority, so "reset" is `Settings{}`'s value for that field — no second
defaults table to keep in sync.

**Conflicts.** None.

---

### 4. Guard the one destructive path

**What.** Turning "Override Global Config" **off** calls `ClearPerGameOverride()`,
which is a bare `std::filesystem::remove` — it deletes `games/<AppId>.json`
outright, immediately, with no confirmation and no undo. A user who toggles it
to see what it does loses every per-game setting they had tuned. Propose:
an inline confirm on the *off* transition only (a second click on a
"remove this game's config?" affordance, not a modal dialog), or keep the file
and mark it inactive.

**Why.** Nielsen #5 (error prevention) and #3. It is the only data-loss path in
the whole overlay, and it is behind a toggle — the control type that most
strongly signals "freely reversible". Valve's equivalent toggle is genuinely
reversible: switching per-game profiles off returns to system defaults, and the
profile is re-creatable, not destroyed by the act of looking. RetroArch's
override system is the cautionary case in the other direction — its per-content
overrides are notoriously confusing partly because settings silently vanish when
they happen to match the parent level.

Note the asymmetry with recommendation 3: I am arguing *against* confirmation
dialogs generally (§2) and *for* one here. The distinction is that everything
else is reversible by re-setting the control; this one is not reversible at all.

**Cost.** S. No modal — the overlay should never go modal (decision #4's input
capture makes a stuck modal genuinely dangerous). A two-step inline affordance
in the existing button language.

**Conflicts.** None. Decision #19 defines the snapshot semantics; it says nothing
about how the *off* transition should be guarded.

---

### 5. Make the two sharpness controls legible against each other

**What.** Three changes, all copy and one state line:
1. Rename by mechanism, not by location: **"Upscale Sharpness"** (Gamescope
   panel — post-upscale, FSR/NIS only) and **"Pre-Sharpen"** (Image panel —
   pre-upscale, any filter). Issue #25's new "Upscaling" tab is the natural home
   for the first and makes the name read correctly in context.
2. Make the cross-reference **reciprocal**. Today only one direction exists:
   `PanelShaders.cpp` says "Pre-upscale — works with any Filter, unlike Sharpness
   (Display panel)"; `PanelDisplay.cpp` says nothing about Pre-Sharpen at all.
3. Add a live state line when *both* are active — e.g. under Upscale Sharpness:
   `also sharpening: Pre-Sharpen 0.80 (Image panel)`. This is the thing that
   actually resolves confusion, because the confusing case is not "which slider
   is which", it is "why does this look over-sharpened when I only turned one up".

**Why.** Decision #12 kept both deliberately and its own recorded consequence is a
UI obligation: "The UI must make the distinction … legible, not collapse them
into one slider." `SPEC.md`'s UI-structure section repeats it: "label both
explicitly to avoid the ambiguity flagged in Feature 2." `PanelShaders.cpp`'s own
file header notes the two "can visibly double up if both are pushed hard — that's
an accepted trade-off". Accepted trade-offs still need to be visible at the point
where the user hits them. This is finishing a decision, not revisiting one.

**Cost.** XS for (1) and (2). S for (3) — the Gamescope panel would need to read
`reshade.pre_sharpen` from the resolved settings, which it already has machinery
for (`config::ResolveEffective`), but it is a new cross-panel read.

**Conflicts.** None; it discharges decision #12's own stated obligation.

---

### 6. Never disable a control without an adjacent reason

**What.** Adopt it as a house rule with a helper (`widgets::DisabledNote()` or an
argument on the existing widgets) so it is easier to follow than to skip, and fix
the case that currently violates it: in `FpsDisplay_DrawSettingsPanel()`, the
Backdrop checkbox and its three sliders auto-disable whenever blend mode is
`additive`, with the reason recorded **only in a source comment** ("Additive
pairs oddly with a filled backdrop… auto-disable rather than let the two silently
combine"). On screen, four controls simply go dead when you change an unrelated
dropdown.

**Why.** The rest of the overlay already does this properly, which is exactly why
the exception is jarring — the sharpness slider explains itself on Linear/Nearest/
Pixel, the Shaders panel explains the HDR gate, the audio panel explains four
distinct unavailable states. `ui-mockup-precise-spec.md` §12 even specifies the
*visual* treatment of a disabled row ("whole row × 34% opacity") using the NIS
slider as its example — the visual side is specified, the explanatory side is a
convention that is followed four times out of five. Nielsen #1 and #9.

Also worth folding in: an explanation on hover is not sufficient for a *disabled*
control, because hovering something that looks dead is not an obvious thing to
do. Disabled reasons stay inline; descriptions go to tooltips (recommendation 2).

**Cost.** XS for the FPS fix, S for the helper and an audit pass.

**Conflicts.** None. Decision #15 (SDR-only) and #23's spirit both already
mandate this behaviour; this generalises it.

---

### 7. Use the slider endpoint labels that already exist — with words, not numbers

**What.** `Widgets.cpp`'s slider implementation already draws min/max scale marks
below the track (`pszMinText`/`pszMaxText`, `Style::ScaleMark`, white @26% — the
measured §7 treatment). But `Widgets.h`'s public `SliderFloat`/`SliderInt`
signatures **do not expose them**, so no caller can pass one and the feature is
unreachable dead code. Expose the parameters, and for the ambiguous scales use
words rather than the numeric bounds:

- Vibrancy strength `-1.0 … 1.0` → `muted` … `vivid` (nothing on screen currently
  tells you negative means desaturate)
- Adaptive Brightness min/max gain → `dimmer` … `brighter`
- Brighten/Darken speed → `instant` … `slow`
- Volume → `mute` … `150%` (the boost region is worth naming)

**Why.** Nielsen #2 and #6, at the cheapest possible price: the drawing code, the
font style, the colour and the geometry are all already written and verified
against the mockup. Ambiguous ranges are the single most common "I do not know
what this does" case that a tooltip alone does not fix, because you are reading
the slider while dragging it, not hovering it.

**Cost.** XS. Two parameters and about a dozen strings.

**Conflicts.** None. It implements §7 as measured.

---

### 8. Mark deferred-effect settings — after auditing which ones actually are

**What.** First establish the truth, then label it. Nearly everything in this
overlay is genuinely live (that is a real achievement — see §1). The known or
suspected exceptions: HDR enable/disable, where the *game* generally has to
renegotiate its swapchain; `display_scale` past the baked atlas range, where text
softens rather than failing (issue #24 documents this precisely); Adaptive
Brightness's adaptation state, which resets on a resolution change (recorded in
decision #14's update, accepted behaviour). Audit these, then attach a small
`needs relaunch` / `needs restart` meta chip to exactly the ones that qualify —
and to nothing else, so the marker keeps its meaning.

**Why.** VS Code's settings editor does exactly this with its per-setting
"Requires reload" indication, and it is the reason nobody files bugs about
settings that "did not work". Here the failure mode is worse than confusion: a
user toggles HDR, sees nothing change, toggles it back, and concludes the control
is broken — when it was working and merely deferred. Nielsen #1.

**Cost.** S for the chip; the audit is the real work and is worth doing on its
own merits regardless of whether the chip ships.

**Conflicts.** None. Note that issue #25's HDR tab will *increase* the surface
that needs this, so it is better decided before that lands than after.

---

### 9. Rename SHADERS → IMAGE; keep GAMESCOPE, and here is why the asymmetry is right

**What.** Retitle the Shaders panel and its dock entry to **IMAGE** (title-bar meta
slot can carry `reshade` for anyone who wants the implementation detail). Leave
issue #25's DISPLAY → GAMESCOPE rename exactly as the user specified.

**Why the rename.** Nielsen #2. "Shaders" names the mechanism. A user who wants
"colours look washed out" or "the picture is soft" has no reason to look under a
menu named after a GPU program type; they *do* have a reason to look under
"Image", which is what every television and every camera calls this menu.
Nothing in `DECISIONS.md` fixes the panel's name — `SPEC.md` calls it "Shaders
panel" descriptively, and issue #25 establishes the precedent that a panel title
is a UI string that can be changed without touching `PanelId`.

**Why *not* the same argument against GAMESCOPE.** By the same heuristic,
"GAMESCOPE" is also jargon — but here the jargon is doing real work that a
plainer word could not. This is a gamescope fork; its users chose it by that
name, and the panel's actual job is "the knobs gamescope itself owns", which is
precisely the distinction that separates it from the ReShade layer next door and
from the two sharpness controls in recommendation 5. A generic "Video" would
*destroy* that distinction. NN/g's rule is "speak the users' language" — for this
audience, that word *is* their language. Keep it.

**Cost.** XS (title string, dock label, doc references).

**Conflicts.** None with a recorded decision. It touches the same file issue #25
touches, so sequence them together rather than in parallel.

---

### 10. Show profile provenance — without turning profiles into live links

**What.** When a profile is applied, record its name in the target config (a
plain informational string, e.g. `meta.last_applied_profile`) and display it in
the Config panel: `last applied profile: FPS`. Explicitly labelled "last
applied", never "current".

**Why, and why not the obvious alternative.** Decision #20 (profile apply is a
one-time copy, not a live reference) is marked ASSUMED — overridable, so it is
the decision most inviting to overturn. **I recommend keeping it.** Making a
profile a live link would create a fifth routing rule on top of the four in
recommendation 1, and would mean that editing a profile silently changes the
settings of every game that ever applied it — the exact class of spooky action at
a distance that makes RetroArch's override hierarchy hard to reason about, and
the opposite of decision #19's deliberately frozen snapshot. Valve's Deck makes
the same choice for the same reason.

But the *cost* of one-time-copy is amnesia: once applied, a config has no memory
of where its values came from, so "did I already apply my FPS profile to this
game?" is unanswerable. The status line answers it for exactly one frame after
you click, then it is gone. A provenance string gives the user the recall benefit
of a link with none of the coupling. It is honest as long as the label says
"last applied" and not "using".

**Cost.** S. One optional schema field, one line of UI, one write on apply.

**Conflicts.** Deliberately *upholds* decisions #19 and #20 rather than
overturning them.

---

## 4. Things I deliberately did not propose

- **A settings search box** — argued against in §2.
- **An "Advanced" tier / progressive-disclosure split** — argued against in §2
  in favour of the tabs #25 already introduces.
- **A 3×3 position grid for notifications or the HUD** — already issues #26/#27,
  and it is the right fix. A spatial grid reading as "where on screen" beats two
  independent vertical/horizontal segmented controls; nothing to add.
- **Resizable / larger / persistent windows** — already #34/#35. Several
  legibility complaints ("I cannot see the whole panel") are window-geometry
  problems, not information-architecture problems, and are already owned.
- **Bigger fonts and controls, wider UI-scale range** — already #23/#24, with
  #24 correctly documenting why the naive range widening would break.
- **Accent hue picker** — already #37, acting on decision #10.
- **A light theme** — decision #8 settled this, with a real reason (no source
  material to design against). Not revisited.
- **Making the overlay non-input-capturing / click-through** — decision #4,
  deliberately modelled on the Steam overlay. Not revisited.
- **The "(M4)" milestone tag currently shown as a user-facing heading in the FPS
  settings panel** — a real (small) leak of internal vocabulary into the UI, but
  issue #27's rename pass rewrites that exact heading. Flagging it here so it
  gets swept up there rather than filed twice.

## 5. One recorded decision that the code has already outgrown

**Decision #23 — "Volume control hides itself when the game's audio node cannot
be identified"** — no longer describes what ships, and what ships is better.
`PanelAudio.cpp` does **not** hide the control: it draws the slider and mute
disabled, states which of four honest reasons applies, and offers an
always-available manual stream picker so a wrong or failed automatic match can be
corrected. The decision's stated rationale ("an honestly-absent control beats a
slider that silently does nothing") is sound, but the implemented behaviour beats
both options in the dilemma as posed — it is neither absent nor silent.

Recommendation: **update decision #23 to record the shipped behaviour**
("disabled and explained, with a manual picker") rather than leave a decision on
record that a future contributor might "restore" by deleting the picker. This is
the only decision I would change, and the change is retrospective bookkeeping,
not a reversal of judgement.

---

## 6. Suggested sequencing

Nothing here is approved; if it is, this ordering minimises rework:

1. **1, 5, 7, 9** — copy, labels and the title-bar badge. Cheap, independent,
   immediately felt, no schema churn. Sequence 9 with issue #25 since both edit
   the same panel titles.
2. **6, 4** — the two correctness-of-communication fixes (silent disabling,
   destructive toggle).
3. **3, 2** — reset affordance, then the tooltip pass. Tooltips last among the
   cheap work because their text should describe the *final* names from step 1.
4. **8, 10** — need an audit and a schema field respectively; also the two most
   worth deferring if the list has to be cut.

Recommendations 1, 5, 6, 7 and 9 together are roughly a day of work and address
the majority of what "not intuitive" means in this overlay.

---

## Sources

Interface guidance:
- Jakob Nielsen, *10 Usability Heuristics for User Interface Design*, Nielsen
  Norman Group, published 1994-04-24, last updated 2024-01-30.
  https://www.nngroup.com/articles/ten-usability-heuristics/
- Jakob Nielsen, *Progressive Disclosure*, Nielsen Norman Group, 2006-12-03.
  https://www.nngroup.com/articles/progressive-disclosure/
- Nielsen Norman Group, *Customization Features* (research report, 46 guidelines
  from usability studies of customization UI).
  https://www.nngroup.com/reports/customization-features/ — cited in §2 as an
  example of guidance that does **not** transfer to a single-user tool.
- Apple, *Human Interface Guidelines — Settings* (patterns).
  https://developer.apple.com/design/human-interface-guidelines/settings
  (content also read via the archived iOS HIG settings page:
  https://codershigh.github.io/guidelines/ios/human-interface-guidelines/interaction/settings/index.html)
- GNOME, *Human Interface Guidelines* — instant-apply preference windows and the
  ">1 second" Apply-button exception (guidance stable from HIG 1.0/2.2.1 through
  the current HIG). https://developer.gnome.org/hig/ ;
  https://p.janouch.name/files/gnome-hig-2.2.1/
- Material Design, *Settings* pattern — secondary text conveys state not
  description; group related settings under a subscreen with a matching label.
  https://m1.material.io/patterns/settings.html and
  https://developer.android.com/design/ui/mobile/guides/patterns/settings

Comparable software:
- Valve, Steam Deck **Quick Access Menu → Performance → Advanced View →
  "Use per-game profile"** (client update introducing per-game performance
  profiles, 2022). Behaviour: system settings by default; toggling on creates a
  profile for the running title; can be toggled off or reset to system defaults
  at any time. https://www.neowin.net/news/latest-steam-deck-update-offers-per-game-performance-profiles/
  ; https://www.deckguy.eu/configure-steam-deck-gaming-mode-performance-options-part-2-quick-access-button-menu/
- ReShade — `ui_tooltip` (added in ReShade 4.0, 2018), `ui_label`, `ui_category`,
  `ui_category_closed`, `ui_category_toggle` uniform annotations; its overlay
  (`runtime_gui.cpp`) renders widgets from `ui_type`/`ui_min`/`ui_max`.
  https://github.com/crosire/reshade-shaders/blob/slim/REFERENCE.md ;
  https://reshade.me/releases/4772-4-0 ;
  https://deepwiki.com/crosire/reshade/6.1-overlay-interface
- Visual Studio Code — settings editor scope model: blue "modified" gutter mark
  and the "Modified in: / Also modified in:" cross-scope indicator; per-setting
  "requires reload". https://code.visualstudio.com/docs/configure/settings ;
  https://github.com/microsoft/vscode/issues/153351
- RetroArch / libretro — core, content-directory and game override hierarchy,
  including the "economy of settings" behaviour where an override matching the
  parent value is removed. https://docs.libretro.com/guides/overrides/ ;
  https://www.retroarch.net/2019/12/using-content-folder-and-core-overrides.html
- MangoHud — text-file configuration (`~/.config/MangoHud/MangoHud.conf`) with
  third-party GUIs (GOverlay, MangoJuice) and the resulting config-drift
  complaints. https://wiki.archlinux.org/title/MangoHud ;
  https://github.com/flightlessmango/MangoHud/issues/1070
- Kevin Giguere, *Create better game settings options (handy checklist)*, Game
  Developer, 2018-11-12 — live preview of image effects, easy reset, tracking
  unapplied changes. https://www.gamedeveloper.com/design/create-better-game-settings-options-handy-checklist-
- `ValveSoftware/gamescope` issue #515, *Make sharpness setting user-friendly* —
  closed as not planned; the reason decision #11's remap is permanently a
  UI-layer responsibility. https://github.com/ValveSoftware/gamescope/issues/515

In-repo (read, not modified): `superdoc/planning/SPEC.md`,
`ui-mockup-precise-spec.md`, `ui-design-guide.md`, `DECISIONS.md`,
`config-system.md`, `runtime-knobs-and-fps.md`, `reshade-shaders.md`,
`pipewire-loudness.md`; `src/Overlay/{Chrome,Widgets,PanelDisplay,PanelShaders,
PanelAudio,PanelConfig,FpsDisplay,Notifications}.cpp`; `src/SettingsOverlay.cpp`;
open issues #19, #23–#37.
