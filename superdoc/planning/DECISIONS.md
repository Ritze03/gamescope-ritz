# Decisions Record — gamescope-ritz overlay project

Date: 2026-08-21

This is a record of decisions made during the planning session, not a design
document. Each entry states what was decided, why, what it forecloses or
requires, and where the evidence lives. The master design spec these
decisions feed into is `superdoc/planning/SPEC.md`; it references this file
rather than repeating its reasoning.

Status tags: `DECIDED` (settled by the user) or `ASSUMED — overridable`
(a reasonable default nobody has confirmed yet).

---

## Scope

### 1. LSFG-VK integration is out of scope for this roadmap
**Status:** DECIDED

**Why:** Two independent, both-real reasons. (a) Licensing: LSFG-VK
([PancakeTAS/lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)) is GPLv3;
gamescope-ritz is BSD-2-Clause, and vendoring GPLv3 code would relicense the
whole binary. (b) Technical: gamescope's presentation loop issues exactly one
composite+present per vblank interval — `gamescope::CVBlankTimer`
(`src/vblankmanager.hpp:27`) drives steamcompmgr's vblank handler to
`paint_all()` (`src/steamcompmgr.cpp:2564`) → one backend `Present()`
(`src/backend.h:205`) → one atomic KMS commit. Frame generation needs *N*
presents per real frame, and there is no multi-present mechanism to hook into.
This is multi-week unsolved R&D, unsolved upstream too.

**Consequences:** The `Lossless.dll` path setting drops out of General
settings for now. Both LSFG planning files (`lsfg-vk-integration.md`,
`lsfg-in-tree-port.md`) stay on disk as preserved research for a future
revisit — not deleted, not acted on.

**Source:** `superdoc/planning/lsfg-vk-integration.md`;
`src/vblankmanager.hpp:27`, `src/steamcompmgr.cpp:2564`, `src/backend.h:205`.

---

### 2. OpenVR backend is out of scope
**Status:** DECIDED

**Why:** Not pursued for this roadmap.

**Consequences:** Removes the ImGui feasibility scout's third-largest
identified risk — VR overlays would have needed a structurally separate
design in the fork's most focus-fragile subsystem (window/input focus
handling). That entire design surface is now avoided.

**Source:** `superdoc/planning/imgui-overlay-feasibility.md`.

---

## Overlay architecture

### 3. In-process ImGui, drawing into gamescope's own compositing pass
**Status:** DECIDED

**Why:** External-overlay windows are unconditionally excluded from focus
candidacy — `GetPossibleFocusWindows()` skips any window with
`isOverlay`/`isExternalOverlay` set:

```
// src/steamcompmgr.cpp:3961
if ( w->isSysTrayIcon || w->isOverlay || w->isExternalOverlay )
{
    continue;
}
```

A separate overlay process could never take keyboard/mouse input without
changing that exclusion, which the project is not doing. In-process ImGui
sidesteps the problem entirely.

**Consequences:** The overlay is not a standalone binary; it lives inside
gamescope's own render/compositing code and its lifecycle is tied to
gamescope's.

**Source:** `superdoc/planning/imgui-overlay-feasibility.md`;
`src/steamcompmgr.cpp:3961` (`xwayland_ctx_t::GetPossibleFocusWindows`).

---

### 4. The overlay takes all input while open; the game gets none
**Status:** DECIDED

**Why:** Modeled on the Steam overlay's own behavior — full input capture,
not a click-through HUD.

**Consequences:** Input capture/release is new territory with no existing
mechanism in the fork to adapt from. Flagged explicitly as the single biggest
schedule risk in the feasibility scout.

**Source:** `superdoc/planning/imgui-overlay-feasibility.md`.

---

### 5. Backdrop blur is dropped; flat translucency plus fade in/out instead
**Status:** DECIDED

**Why:** Backdrop blur is the signature visual of the handoff design
language, but ImGui has no native blur primitive. Rather than build custom
blur-compositing machinery, the user substituted a fade in/out on
overlay open/close — their own proposal, offered in place of blur.

**Consequences:** Panels render as flat, near-black translucent rectangles
(no blur-behind). The open/close transition is an added requirement not in
the original handoff.

**Source:** `superdoc/planning/imgui-overlay-feasibility.md`,
`superdoc/planning/ui-design-guide.md`.

---

### 6. Overlay toggle hotkey is `Ctrl + Shift + O`
**Status:** DECIDED

**Why:** Chosen deliberately to avoid colliding with the Steam Deck's
existing `Super+U/I/O` filter-cycling bindings.

**Consequences:** The overlay's global-hotkey handler must bind specifically
to `Ctrl+Shift+O`, and any future rebinding UI should keep the Steam Deck's
`Super+U/I/O` set in mind as a collision to avoid, not just this one key.

**Source:** Planning session (no dedicated sibling file).

---

## Look

### 7. Dark-only "glass instrument" visual language
**Status:** DECIDED

**Why:** Taken directly from the design handoff: near-black translucent
panels, 1px hairline borders, flat/square controls, cyan default accent,
IBM Plex Sans for prose, IBM Plex Mono (tabular figures) for all numeric
readouts.

**Consequences:** No light theme is part of this language (decision 8).
Every control built for the overlay should read against this palette by
default.

**Source:** `superdoc/planning/ui-design-guide.md`.

**Update (2026-08-22) — typeface swapped to Geist (issue #53):** the
Sans/Mono role split itself is unchanged, but the bundled family is now
Geist Sans/Geist Mono (SIL OFL 1.1, same license family as Plex), not IBM
Plex. See `src/Overlay/Fonts.cpp`'s top-of-file comment and
`superdoc/planning/ui-mockup-precise-spec.md`'s Typography table for the
font-file and size details.

---

### 8. Dark theme only, no light theme
**Status:** DECIDED

**Why:** The design handoff is dark-only, so a light palette would have to
be invented with no source material behind it.

**Consequences:** No light-theme variant is planned or budgeted for; any
future request for one starts from zero design material, not an adaptation
of existing tokens.

**Source:** `superdoc/planning/ui-design-guide.md`.

---

### 9. Icons are generated as SVG, in the handoff's icon style
**Status:** DECIDED

**Why:** Matches the handoff's existing icon treatment; SVG keeps icons
crisp at the overlay's various UI scales and themeable via the accent color.

**Consequences:** No bitmap icon assets; icon set needs to be authored/traced
to match the handoff style rather than sourced from a generic icon pack.

**Source:** `superdoc/planning/ui-design-guide.md`.

---

### 10. Accent color is user-selectable; uncovered controls invented in-style
**Status:** ASSUMED — overridable

**Why:** The handoff mockups fix cyan as the default accent but don't specify
every control type the overlay will need (dropdowns, text inputs,
scrollbars). Treating the accent as a user setting is a natural extension of
"cyan default," and gaps get filled by extrapolating the established
language rather than inventing a new one.

**Consequences:** Any new control added later should be checked against the
existing glass-instrument rules (hairline borders, flat/square, mono
numerals) rather than styled ad hoc.

**Source:** `superdoc/planning/ui-design-guide.md`.

---

## Settings behaviour

### 11. Sharpness slider is single and auto-corrected across filters
**Status:** DECIDED

**Why:** FSR and NIS remap gamescope's shared `0–20` sharpness value in
*opposite* directions (`sharpness/10` for one, `(20-sharpness)/20` for the
other), so a naive single slider would invert its own meaning when the user
switches filters. The UI instead presents one slider where higher always
means sharper, and the code applies the correct per-filter remap underneath.
Upstream `ValveSoftware/gamescope` issue #515 ("Make sharpness setting
user-friendly") confirms the inversion is real and was closed as not
planned — this is a permanent UI-layer responsibility, not something a
future upstream fix will absorb.

**Consequences:** The remap logic must live in the overlay/config layer, not
be assumed away; it will need to stay correct if gamescope's own upscale
filter set changes.

**Source:** `superdoc/planning/runtime-knobs-and-fps.md`
(lines ~116–128); upstream
[ValveSoftware/gamescope#515](https://github.com/ValveSoftware/gamescope/issues/515).

---

### 12. Both sharpness paths exist: gamescope's built-in + a separate pre-upscale ReShade sharpen
**Status:** DECIDED

**Why:** User's own words: *"gamescope sharpening sometimes gives issues
with certain games."* Rather than replace gamescope's post-upscale
RCAS/NIS sharpness with a ReShade equivalent, both stay available: gamescope's
existing post-upscale sharpening, and a separate pre-upscale ReShade
sharpen effect as an alternative/fallback.

**Consequences:** Two sharpness controls exist simultaneously and can
interact (a game could have both applied). The UI must make the distinction
between "gamescope's own sharpening" and "the ReShade sharpen effect"
legible, not collapse them into one slider.

**Source:** `superdoc/planning/runtime-knobs-and-fps.md`;
`superdoc/planning/reshade-shaders.md`.

---

### 13. ReShade effects ship as one combined `.fx`, each effect gated by its own on/off uniform
**Status:** DECIDED

**Why:** gamescope's ReShade manager loads one effect at a time, and
*switching* effects forces a synchronous FX parse + SPIR-V compile + Vulkan
pipeline build inline on the steamcompmgr render thread, which hitches the
vblank loop. The alternative considered — extending the manager to chain
multiple effects — was rejected. Gating effects behind uniforms inside a
single always-loaded effect makes toggling cost a uniform write instead of a
recompile. Record this reasoning carefully: it is the kind of thing someone
later "simplifies" away by reaching for the seemingly-cleaner multi-effect
manager, reintroducing the recompile hitch.

**Consequences:** The combined `.fx` must be structured so a third effect
can be appended later without restructuring it. The single-effect-at-a-time
constraint in the ReShade manager itself is not being fixed — see
"Still open."

**Source:** `superdoc/planning/reshade-shaders.md`.

---

### 14. Adaptive Brightness is deferred; Vibrancy and Sharpness ship first
**Status:** DECIDED — landed on master 2026-08-22, see updates below.

**Why:** Adaptive Brightness needs persistent inter-frame texture state that
no shipped effect in the repo exercises, so it carries real risk relative to
Vibrancy and Sharpness.

**Consequences:** Adaptive Brightness becomes a later milestone (M9a/M9b in
`SPEC.md`) opening with a throwaway prototype that proves the persistence
assumption before real engineering time is spent on it. It is explicitly off
the critical path for v1.

**Source:** `superdoc/planning/reshade-shaders.md`; `superdoc/planning/SPEC.md`.

---

**Update (2026-08-21) — M9a spike result: persistence confirmed, plus two
unrelated findings.** Run on an experimental branch (issues #17/#18), on real
hardware (RADV/AMD Radeon RX 7900 XTX), not inferred from reading barrier
code.

**Method:** a throwaway `.fx` (never shipped) with a persistent 1×1 `R32F`
texture, incremented one way or another each real frame, read back the
following frame and displayed as a brightness ramp. Launched via
`--reshade-effect` against `vkcube` (renamed to a binary name outside
`lsfg-vk`'s per-game profile list — `vkcube`'s own default profile crashes
the process even with `DISABLE_LSFGVK=1` set on the parent, since gamescope's
child-spawn passes env through but the profile still matches on `exe` name).
Screenshots taken via `gamescopectl screenshot` at timed intervals with a
temporary `HOME`/`XDG_CONFIG_HOME`, decoded with Pillow.

**Result: persistence survives.** A `RenderTarget` texture's contents
carry over from one `vulkan_composite()`/`execute()` call to the next,
confirmed two ways: (a) a value written one frame and read the next produced
a visibly progressing, wrapping ramp across three screenshots taken seconds
apart (88 → 195 → 139, consistent with a mod-64 cycle), with the pipeline
compiling exactly once (no reinit destroying the texture); (b) later, in the
real implementation, a synthetic time-varying brightness signal produced a
gain that was demonstrably *not* a constant multiplier (the with/without
luminance ratio varied 9.26→1.44 across a sweep) — proof the persisted,
self-sampled `texAdaptedLuminance` value genuinely evolves frame to frame.

**Two unrelated findings surfaced while isolating this, both worth recording
so nobody re-discovers them the hard way:**

- **A shader declaring zero `uniform` variables crashes RADV during pipeline
  creation**, regardless of what its passes do. Root cause:
  `m_module->total_uniform_size` is 0, so `ReshadeEffectPipeline::init()`
  calls `vkCreateBuffer` with `size = 0` for the uniform buffer
  (`reshade_effect_manager.cpp` "Create Uniform Buffer" block) — invalid per
  the Vulkan spec, and RADV segfaults on it rather than erroring cleanly.
  Confirmed by isolation: two structurally identical throwaway shaders,
  one with a (used or unused) `uniform` declared and one without, only the
  uniform-free one crashed; adding a dummy `uniform` to the *same*
  self-sampling shader that had just crashed made it run clean. **This is a
  non-issue for any real effect** — every effect in `gamescope-ritz.fx`
  declares several uniforms — but is a genuine latent crash if anyone ever
  authors a uniform-free ReShade effect for this fork. Not fixed here (out
  of scope for this experimental branch); worth a defensive check in
  `reshade_effect_manager.cpp` (skip/guard the buffer create when
  `total_uniform_size == 0`) if this ever bites a real effect.
- **Self-sampling (a pass reading, via a sampler, the very texture that is
  that pass's own `RenderTarget`) does not crash and reads sane values**,
  once the zero-uniform issue above is out of the way. This was initially
  mistaken for the crash cause; isolating the two variables separately
  showed self-sampling alone is fine on this driver. This directly
  simplified the Adaptive Brightness implementation: no ping-pong/relay
  buffers needed, just one persistent texture, read-blend-write in a single
  pass — the standard pattern real ReShade auto-exposure community shaders
  already use.
- **(Carried over, still true, not new.)** The pass with no explicit
  `RenderTarget` (the implicit default output) must be the *last* pass in a
  technique's pass list, or `ReshadeEffectKey`'s single-entry cache goes
  unstable and the pipeline recompiles every single frame instead of caching
  once — confirmed by counting `"Compiling pass"` log lines (1 compile event
  with the implicit-output pass last across an 8s run; continuous
  recompilation with it reordered earlier). `gamescope-ritz.fx`'s own header
  comment had already anticipated this ("if it's the new last pass") before
  it was empirically confirmed here.

**Resolution changes:** unchanged from the original risk note below — a
resize still creates a new `ReshadeEffectKey` (different `bufferWidth`/
`bufferHeight`), rebuilding the pipeline from scratch and resetting
`texAdaptedLuminance` to its cleared (zero) initial state. This is accepted,
not fixed, consistent with the original risk assessment. Issue #20's fix
(`FrameInfo_t::bBaseLayerReshaded`) is directly relevant here and was
double-checked: on the preemptive-upscale path, ReShade — and therefore
Adaptive Brightness's adaptation state — only updates once per real frame
(the second, post-upscale `vulkan_composite()` call skips ReShade entirely),
so there is no double-counting or resolution-mismatch hazard from that path;
the only resolution-driven reset is the ordinary one described above.

**Consequence: M9b (implementation) proceeded** on the same experimental
branch — see `reshade/Shaders/gamescope-ritz.fx` and
`src/Overlay/PanelShaders.cpp`. Stays experimental/unmerged pending further
real-world (non-`vkcube`) testing; see `SPEC.md` M9a/M9b for status.

---

**Update (2026-08-22) — M9b landed on master; validated against real,
time-varying content, not just `vkcube`.** The experimental branch's own
verdict was honest that it had "validated only against vkcube with a
synthetic pulse, never against a real game's natural brightness variation."
This pass closes that gap before merge.

**Rebase notes:** `reshade/Shaders/gamescope-ritz.fx`'s reserved M9 section
(the file's own header comment promised appending needed no changes above
it) held exactly as designed — the three-pass technique, the
`texPreSharpenOut` named-target change, and all seven `source`-tagged
uniforms carried over unmodified from the experimental branch. The Shaders
panel (`src/Overlay/PanelShaders.cpp`) had been restyled since the branch
was cut (M8 part 2's `widgets::Toggle`/`widgets::SliderFloat`, M8 part 3's
`chrome::BeginPanelWindow`) — `DrawAdaptiveBrightnessGroup()` was rewritten
against the current widget API rather than reintroducing the old raw-ImGui
styling; `ConfigSchema.h`'s `adaptive_brightness` fields needed zero changes,
having been reserved with the exact field names/ranges the branch already
used.

**Validation method:** a 24s synthetic-but-really-decoded MP4 (`ffmpeg`
`testsrc2` alternating `eq=brightness=-0.45`/`+0.35` in 6s segments, looped)
played by a real `mpv` client through the full pipeline — Wayland → Xwayland
→ gamescope base layer → ReShade's 3-pass technique → FSR upscale →
composite — at `-W 1920 -H 1080 -w 1280 -h 960 -S stretch --filter fsr
--sharpness 5`, `DISABLE_LSFG=1`, a temporary `XDG_CONFIG_HOME` pre-seeded
with `adaptive_brightness.enabled`. This is a materially different (and
more honest) test than the branch's own vkcube-plus-synthetic-pulse: real
decode timing, real scene-cut jumps, real upscale/compositing in the loop.
`gamescopectl screenshot` was used with **screenshot type 3
(`full_composition`)**, not the default type 1 (`base_plane_only`) —
confirmed by reading `steamcompmgr.cpp`'s screenshot handler that
`base_plane_only` repaints the focused window at render resolution via
`paint_window()`/`vulkan_screenshot()` directly, bypassing
`vulkan_composite()` entirely, which is where ReShade actually runs; using
the default type would have silently screenshotted *pre-ReShade* content.
Worth remembering next time this needs re-checking.

**Result:** eight `full_composition` screenshots spaced 3s apart (one full
24s loop), mean luminance measured with ImageMagick (`-colorspace Gray
-format "%[fx:mean]"`):

| | dark-scene mean | bright-scene mean | range | ratio |
|---|---|---|---|---|
| Adaptive Brightness off | 0.605 | 0.864 | 0.259 | 1.43× |
| Adaptive Brightness on (up/down speed 0.5s, target 0.5) | 0.681–0.698 | 0.746–0.768 | ~0.08 | ~1.1× |

The on/off luminance range compresses roughly 3× on genuinely varying real
content, in the direction the design predicts (both scenes pulled toward
the shader's internal 5×5-grid-measured target, not toward the same
full-frame mean ImageMagick measures — the two disagree in absolute value
by design, only the *compression* is the claim being checked here). Compile
log ("Compiling pass") stayed at exactly 4 lines (one per pass) across the
whole sequence, both with the effect on and off — confirming M9b's own
per-pass-uniform-gating design still holds and no per-frame recompile
regressed back in during the M8 restyle.

**Ready-to-ship assessment:** ready to ship as the same "experimental"-
labeled effect the branch shipped it as (the Shaders panel group still says
so) — the persistence/self-sampling/recompile risks are real-hardware-
confirmed and the adaptation direction is now confirmed against real,
scene-cut-driven content, not just a synthetic pulse. Still provisional in
the sense M9a/M9b always were: only tested against one driver (RADV/AMD),
one synthetic-content video, and a `-w 1280 -h 960` render size — not
against an actual game's organic brightness variation, and not across
vendors. Resolution-change reset-to-zero (documented above) is accepted,
unchanged, un-fixed behaviour, not a regression.

---

### 15. Effects are SDR-only for v1
**Status:** DECIDED

**Why:** HDR-correct color math in both paths is significantly more work
with more ways to be subtly wrong.

**Consequences:** Effect controls grey out with an explanation when HDR is
active. This is a deliberate v1 limitation, not an oversight, and should be
documented as such wherever effects are surfaced in the UI.

**Source:** `superdoc/planning/reshade-shaders.md`.

---

### 16. FPS headline number is the game's own frame rate, not the composited/presented rate
**Status:** DECIDED

**Why:** All three relevant clocks — game frametime, composited frametime,
and display refresh — are already computed and available in
`mangoapp_msg_v1` (`src/mangoapp.cpp:20-56`: `app_frametime_ns`,
`visible_frametime_ns`, etc.), so choosing the game's own frametime as the
headline is a presentation choice, not a data-availability constraint.

**Consequences:** The overlay's FPS readout must specifically source the
game-frametime field, not the composited/presented one, even though both are
on hand; the other clocks remain available for secondary/detail display.

**Source:** `superdoc/planning/runtime-knobs-and-fps.md`;
`src/mangoapp.cpp:20` (`mangoapp_msg_v1` struct),
`src/mangoapp.cpp:53-56`.

---

### 17. FPS counter is built in ImGui, reusing mangoapp's timing math but not its rendering
**Status:** DECIDED

**Why:** mangoapp is an external process that gamescope cannot style —
its font size, backdrop, and blending are outside gamescope's control. That
structurally rules out meeting the glass-instrument visual requirements
(decision 7) through mangoapp's own renderer, so only the underlying
frametime computation is reused; the rendering is native ImGui inside the
overlay.

**Consequences:** A second, in-tree FPS-rendering path now exists alongside
mangoapp's; the two must not visually collide when both could theoretically
run.

**Source:** `superdoc/planning/runtime-knobs-and-fps.md`; `src/mangoapp.cpp`.

---

## Config

### 18. Config lives in `~/.config/gamescope-ritz/`
**Status:** DECIDED

**Why:** Standard XDG-style per-user config location, split into
`global.json`, `profiles/<ProfileName>`, and `games/<AppId>.json`.

**Consequences:** Config-system design (file formats, merge order) is
scoped to this directory layout; anything referencing `games/<AppId>.json`
depends on the app id resolution order in decision 21.

**Source:** `superdoc/planning/config-system.md`.

---

### 19. "Override Global Config" takes a full snapshot, not a diff
**Status:** DECIDED — amended 2026-08-22, see update below.

**Why:** When enabled for a game, the per-game file captures every setting
value at that moment; subsequent changes to the global config do not
propagate to that game afterward. Default is off, and if a game never
enables the override, no per-game file is created at all — avoiding config
sprawl for games that don't need it.

**Consequences:** Per-game files can silently drift out of sync with global
defaults once created — that's the intended behavior, not a bug, but any UI
surfacing config state should make "this game is snapshotted and frozen"
visible.

**Source:** `superdoc/planning/config-system.md`.

---

**Update (2026-08-22) — turning the override off no longer deletes
`games/<AppId>.json`, and turning it back on now restores that file instead
of re-snapshotting. Issue #43 (config-UI review): "Override Global Config"
off was found to immediately call `remove()` on the per-game file — a
destructive action hidden behind an ordinary toggle, with no confirmation
and no undo. The user's own instruction: "It shouldnt do that. There can be
a button for it, but never delete configs automatically."

**Status: DECIDED**, by the user.

**What changed:**
- Disabling the override (`ConfigManager.cpp`'s `ClearPerGameOverride`) now
  flips the file's own `override_global` field to `false` **in place** and
  leaves the rest of the file untouched, instead of deleting it. The file
  stops being authoritative (`LoadPerGameOverride`/`ResolveEffective` fall
  through to global exactly as before), but the values survive on disk.
- This reopens a question decision 19 (above) didn't anticipate: with a file
  now able to survive a disable, what should re-enabling do when one already
  exists? Two options were considered — (a) restore the existing file's
  values, or (b) re-snapshot from whatever is currently effective (the
  letter of decision 19, "captures every setting value at that moment").
  Option (b) was rejected: it would silently discard the saved per-game
  values the very first time the user toggled override off and back on,
  reintroducing the same data loss issue #43 was about, one step later.
- **The user confirmed directly: "It should just load those settings."**
  `EnableOverride()` (`PanelConfig.cpp`) now checks
  `HasSavedPerGameConfig()` first; if a saved file exists, it calls the new
  `RestorePerGameOverride()` (flips `override_global` back to `true` in
  place, values untouched) instead of snapshotting. Decision 19's original
  full-snapshot behavior is now specifically the **first-time** path — it
  only runs when no saved file exists yet for this game.
- A new, explicit, user-confirmed **"Delete Saved Config..."** button
  (Per-Game tab) is now the only path in the entire app that can delete
  `games/<AppId>.json` — gated behind a confirmation modal
  ("This permanently deletes ... This cannot be undone."), and implemented
  by the new `DeletePerGameOverride()`, which refuses any app id containing
  a path separator or resolving outside `GamesDir()` and never touches
  `global.json` or anything under `profiles/`.
- The Per-Game tab now shows an explicit line — "A saved config exists for
  this game (`games/<id>.json`). Turning Override back on loads it — it
  won't be re-created from global." — whenever the override is off but a
  saved file still exists, so the restore behavior above doesn't look like
  a bug the next time the user flips the toggle back on.

**Consequences:** "Override Global Config" is no longer purely a live
routing switch — turning it off is now a state-preserving pause, not a
teardown. `games/<AppId>.json` can now exist in a state its filename alone
doesn't reveal: present on disk but inactive (`override_global: false`).
Every reader must keep checking the flag (as `LoadPerGameOverride` already
does) rather than inferring activeness from the file's mere existence
(as `HasSavedPerGameConfig`/`ListGameIds` correctly still do, for different
questions). Config sprawl is a little more likely now (a disabled-and-never-
re-enabled game's file lingers instead of vanishing) — accepted, since
silent data loss is worse.

**Source:** `superdoc/planning/config-system.md`; `src/Overlay/PanelConfig.cpp`;
`src/Config/ConfigManager.{h,cpp}`; issue #43.

---

### 20. Applying a profile copies values in once; it is not a live reference
**Status:** ASSUMED — overridable

**Why:** Kept consistent with the snapshot semantics of decision 19 — a
profile "applied" to global or per-game config should behave the same way a
per-game override does: a one-time copy, not a link that keeps updating.

**Consequences:** Editing a profile after it's been applied somewhere does
not retroactively change anything that already applied it; re-applying is
required to pick up profile edits.

**Source:** `superdoc/planning/config-system.md`.

---

### 21. App id resolution order
**Status:** DECIDED

**Why — the order:**
1. `GS_RITZ_APPID` (explicit override, always wins)
2. `STEAM_COMPAT_APP_ID` (set for Proton/Windows titles)
3. `SteamAppId`, but only when it is nonzero — it can legitimately be
   the literal string `"0"` for some launch paths, which must not be
   mistaken for "app id zero"
4. The basename of `STEAM_COMPAT_DATA_PATH` (the `compatdata/<appid>/`
   directory is itself named by app id, a documented fallback for when
   `SteamAppId` is `"0"`)
5. Nothing found → global config only, no per-game resolution

**Why — the nuance that settled it:** There is **no environment variable
literally named `AppId`**. `AppId=<n>` is a *command-line argument* nested
inside Steam's `reaper` wrapper invocation, not an env var — confirmed by
this fork's own existing code: `get_appid_from_pid()`
(`src/steamcompmgr.cpp:5285`) already scrapes `/proc/<pid>/stat`/cmdline for
exactly this, and `new_win->appID = get_appid_from_pid(...)`
(`src/steamcompmgr.cpp:5465`) is where it's consumed today. The new
resolution order is layered on top of, not a replacement for, this existing
mechanism.

**Why — the topology split (also settled, also load-bearing):** Env vars
(`GS_RITZ_APPID`, `STEAM_COMPAT_APP_ID`, `SteamAppId`) only reach gamescope
when gamescope itself is the launch-option wrapper, i.e.
`gamescope -- %command%`. In a persistent-session topology, gamescope's
process predates the game entirely — it is already running when the game
launches — so none of those env vars are inherited, and only a
post-startup window/property scrape (of the kind `get_appid_from_pid`
already performs) can resolve the app id in that topology.

**Consequences:** Two distinct signals exist — the env-var read (wrapper
topology) and `get_appid_from_pid`'s process/window scrape
(persistent-session topology) — but only the first is wired into config
resolution. `ResolveAppId()`/`SessionAppId()` resolve once, at the very top
of `main()`, purely from env vars; the scrape is never consulted by the
config system. In the persistent-session topology this means resolution
correctly, deliberately lands on "no app id" and every setting falls
through to `global.json` — not a gap, per `config-system.md`'s "mid-session
appid arrival is out of scope for v1" and `ConfigManager.h`'s
`SessionAppId()`'s "no live app-id reload" (a resolved id never changes
mid-process, so there is nothing to hot-switch to even if the scrape were
wired in — and hot-switching was independently rejected as confusing,
settings visibly changing after launch). `PanelConfig.cpp`'s
`DrawPerGameTab()` shows this state honestly ("No game identified for this
session ... All changes made in the other panels are going to global.json")
whenever `SessionAppId()` is `std::nullopt`.

**2026-08-22 verification (closes "not yet tested"):** Built and ran with
`--ritz-dump-config` under a temporary `XDG_CONFIG_HOME`, both topologies
simulated directly, using the user's offered app id `3746030`:
- Wrapper topology (`STEAM_COMPAT_APP_ID=3746030` set before exec) →
  `resolved_app_id: "3746030"`, reads/writes `games/3746030.json` only.
- Persistent-session topology (none of the four env vars set) →
  `resolved_app_id: null`, falls through to `global.json`, creates nothing.
No input in either topology produced a *wrong* app id — the dangerous case
(silently attaching one game's settings to a different game) does not
occur; the only failure mode possible is the honest "no id" degradation.
Also confirmed by code reading: `gamescope::config::` is never referenced
anywhere in `steamcompmgr.cpp` — the scrape and the config system are
completely disjoint today. Regression tests added in
`tests/test_config.cpp` ("persistent-session topology ... resolves to
nothing, not a stale id"; "a bare \"AppId\" env var is never read"; the
3746030 end-to-end wrapper-topology test) guard both claims going forward.

**Source:** `superdoc/planning/appid-detection.md`;
`src/steamcompmgr.cpp:5285` (`get_appid_from_pid`),
`src/steamcompmgr.cpp:5465` (call site); `src/Config/AppId.cpp`;
`src/main.cpp` (`ResolveAppId`/`ResolveEffective` call site, top of `main()`);
`tests/test_config.cpp`.

---

## Audio

### 22. Volume control shells out to `wpctl` for v1, not the raw PipeWire API
**Status:** ASSUMED — overridable

**Why:** WirePlumber's `wpctl --pid` flag already implements the exact
primitive needed — "given a PID, find and control its audio node(s)" — as a
first-class feature (`wpctl set-volume ID VOL[%] --pid`,
`wpctl set-mute ID 1|0|toggle --pid`), so shelling out is genuinely the
least code: no new PipeWire registry-listener bookkeeping, no
threading-affinity handling, reusing gamescope's existing
`Process::SpawnProcess`. The tradeoff (a runtime dependency on `wpctl` not
build-time-checked the way `libpipewire-0.3` is, plus poll-based readback
instead of push events) was accepted for v1.

**Consequences:** A missing `wpctl` binary becomes a silent-failure mode this
codebase doesn't otherwise have in the audio path; volume readback is
polling-based, not event-driven, for v1.

**Source:** `superdoc/planning/pipewire-loudness.md` (esp. lines ~81-84,
~258-260).

---

### 23. Volume control hides itself when the game's audio node cannot be identified
**Status:** ASSUMED — overridable

**Why:** Proton titles commonly run through `pressure-vessel`/`bwrap`
sandboxing (Steam Linux Runtime "soldier"/"sniper" containers), which can
put the real game process in a different PID namespace than the one
gamescope sees, breaking `wpctl --pid` matching. An honestly-absent control
beats a slider that silently does nothing.

**Consequences:** Some games — specifically sandboxed Proton titles where
PID-namespace matching fails — will simply not show a volume slider at all
under v1's `wpctl`-based approach; this is a known, accepted gap tied
directly to decision 22.

**Source:** `superdoc/planning/pipewire-loudness.md` (esp. lines ~18-22,
~94-96).

---

## Process

### 24. Testing uses `vkcube`; development stays developer-normal
**Status:** DECIDED

**Why:** `vkcube` is a minimal, always-available Vulkan test target — no
need to stand up a full game for iteration. Regular commits and issue
tracking follow ordinary project norms; nothing outside the project's own
scope gets touched during this work.

**Consequences:** Test/validation steps in the eventual spec should default
to `vkcube` unless a decision explicitly calls for a real game (e.g. app id
`3746030` per decision 21's still-untested case).

**Source:** Planning session (no dedicated sibling file).

---

### 25. Toast notifications: placement is global-only, muting is per-game
**Status:** DECIDED

**Why:** The task's own design split. Where toasts appear on screen is a
personal preference about the player's physical setup (monitor layout,
where their eyes naturally rest) that has nothing to do with any one
game, so it lives on `OverlaySettings` (`ConfigSchema.h`) - the same
process-level, global-only slot `fade_ms` already occupies, exempted from
`SettingsToJson`'s per-game/profile snapshot the same way (`bIncludeOverlay`).
Whether toasts should appear *at all*, by contrast, is naturally a
per-game decision (a competitive shooter's player wants silence, a strategy
game's player doesn't), so `NotificationSettings::muted` is an ordinary
per-layer field - it rides along in a full per-game snapshot exactly like
`fps_display.enabled` does, resolved by the config system's existing
two-level (global vs. per-game full-snapshot) layering (decision 19) with
no special-casing needed.

**Consequences:** A game with "Override Global Config" enabled still uses
the *global* placement - its own snapshot file never contains an `overlay`
object at all, so `Notifications.cpp` must read placement from
`LoadGlobal()` directly rather than from the per-game-resolved effective
`Settings`, or it would silently fall back to the compiled-in default
instead of the user's real choice. `tests/test_config.cpp` covers this
resolution explicitly (`"notification placement is global-only..."`,
`"notification muting resolves per-game override vs. global..."`).

**Source:** Task brief (this milestone); `src/Overlay/Notifications.h`.

---

## HUD Layouts

### 26. A layout is referenced by name, resolved at use time — never copied through `ApplyProfile()`
**Status:** DECIDED

**Why:** The manual-placement HUD-layout rework (replacing the old auto-anchored
module stack) needs "editing a layout updates it everywhere it's used" — named layouts
like "Simple"/"Advanced" are shared, standalone entities a profile or game merely
*points at*. That is the exact opposite of decision 20's `ApplyProfile()` semantics (a
deliberate one-time copy, so editing a profile afterward never retroactively changes
anything that already applied it). Routing layout content through the profile-copy
path would have silently frozen a layout's placement at apply-time, defeating the
whole point of a shared, editable layout.

**Consequences:** `FpsDisplaySettings::layout_name` (a plain string) is layered
globally/per-profile/per-game exactly like every other `fps_display` field, and
`ApplyProfile()` copies that *name* like any other field (a one-time "point this
profile-applier at layout X" choice, consistent with decision 20). But the layout's
own *content* — module positions, per-module/per-row toggles — is never baked into a
`Settings` object at all, at any layer, at any point; it is stored only in its own
`layouts/<name>.json` (parallel to, not inside, `profiles/`/`games/`) and resolved
from the name at use time via `ConfigManager::ResolveLayoutCached()`. A layout that
no longer exists (deleted, renamed, mistyped) degrades to a completely valid empty
`HudLayout{}` — "render nothing" — rather than an error or stale data, and the store
ships with **no default layouts at all**: an unset `layout_name` is exactly that same
valid empty state. Content toggles that used to live on `FpsDisplaySettings`
(`cpu_enabled`/`gpu_enabled`/`media_enabled`/`fps_enabled`/`graph_enabled`/
`percentiles_enabled`/`frametime_enabled`/`fps_label_enabled`) conceptually move to
the layout (`HudLayoutModule::enabled`, `HudLayoutFpsModule`'s sub-row toggles) — the
originals stay on `FpsDisplaySettings` for Phase 0 only, since `FpsDisplay.cpp` still
reads them and retiring them is a later, separate phase's job.

**Source:** Task brief (Phase 0 of the HUD-layout rework);
`superdoc/architecture/hud-layouts.md` (full design); `src/Config/ConfigSchema.h`
(`HudLayout`/`HudLayoutModule`/`HudLayoutFpsModule`/`FpsDisplaySettings::layout_name`);
`src/Config/ConfigManager.{h,cpp}`; `tests/test_config.cpp`.

---

## Still open

One genuine open item remains: the ReShade manager's single-effect-at-a-time
constraint. Decision 13 works around it (gating effects behind uniforms
inside one combined `.fx`) rather than fixing it — extending the manager to
properly chain/compose multiple effects remains a possible future
improvement, not ruled out, just not pursued now.
