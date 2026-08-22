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
**Status:** DECIDED

**Why:** Adaptive Brightness needs persistent inter-frame texture state that
no shipped effect in the repo exercises, so it carries real risk relative to
Vibrancy and Sharpness.

**Consequences:** Adaptive Brightness becomes a later milestone (M9a/M9b in
`SPEC.md`) opening with a throwaway prototype that proves the persistence
assumption before real engineering time is spent on it. It is explicitly off
the critical path for v1.

**Source:** `superdoc/planning/reshade-shaders.md`; `superdoc/planning/SPEC.md`.

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
**Status:** DECIDED

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

## Still open

One genuine open item remains: the ReShade manager's single-effect-at-a-time
constraint. Decision 13 works around it (gating effects behind uniforms
inside one combined `.fx`) rather than fixing it — extending the manager to
properly chain/compose multiple effects remains a possible future
improvement, not ruled out, just not pursued now.
