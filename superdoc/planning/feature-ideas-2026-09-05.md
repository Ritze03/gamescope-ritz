# Feature ideas — 2026-09-05 (round 2, item 5)

Request: *"think of some more filters and features in general, that would be nice to
have."* Prioritised, grounded in what exists. Companion to
[profiles-concept.md](profiles-concept.md); the earlier
[feature-ideas-research.md](feature-ideas-research.md) (friends list, MPRIS, system
load) is not repeated. Sizes: S = a day or less, M = a few days, L = a week plus.
No per-frame numbers below have been measured; costs are reasoned from the pass shape.

## Where things sit

- **Native pre-pass** (`src/shaders/cs_effects_layer0.comp` + `cs_effects_measure.comp`,
  `effects_common.h`): one 8x8 compute dispatch over the *game layer at source
  resolution*, encoded sRGB in and out, SDR only, before FSR/NIS/blit. A per-pixel
  effect is a few lines in `grade()` and a flag bit; a neighbourhood effect adds taps.
  Any enabled effect forces a full composite (no direct scanout) -- already true today.
  The compositor's own HUD, crosshair and toasts are composited *after* this pass, so
  nothing here ever touches them.
- **Colour management** (`color_helpers.cpp`, `steamcompmgr.cpp:update_color_mgmt`):
  a per-EOTF shaper + 3D LUT applied to the *whole output*, HUD included. It already
  has a night mode (`nightmode_t`: amount/hue/saturation, X11 atom
  `GAMESCOPE_COLOR_NIGHT_MODE`, `steamcompmgr.cpp:7283`) and a `.cube` "look"
  loader (`LoadCubeLut()`, `g_ColorMgmtLooks`, `set_look` ConCommand
  `wlserver.cpp:1986`, `gamescope_control.set_look`). Neither has a settings row.
- **HUD layer** (`FpsDisplay_AddLayer`, `Notifications::AddLayer`, `steamcompmgr.cpp:3247`),
  the **Shell**, and the **console** (`overlay_e2_set/get/select/palette`,
  `Shell.cpp:599-644`, reachable via `gamescopectl`).

## Filters

| # | Filter | For the player | Fit | Size | Cost/frame | Honest take |
|---|---|---|---|---|---|---|
| F1 | **Levels: black point / gamma / white point** | Lift crushed blacks or tame a washed-out game without touching the monitor; gamma is the "dark maps" knob Shadow Control only half covers. | Three floats in `grade()` after Shadow Control: `c = pow((c - lo) / (hi - lo), 1/gamma)`. One row, 3 params. | S | Negligible (per-pixel ALU). | Real, both audiences. Prefer one Levels row over 9-param lift-gamma-gain; the six-Param budget per row (`PanelShaders.cpp`) fits it. |
| F2 | **Colour temperature / white point** (Kelvin) | Warmer or cooler picture; the competitive use is a fixed white point across games. | Host maps Kelvin to an RGB multiplier (a small table), shader multiplies. Or: expose the existing colour-management night mode instead (F3). | S | Negligible. | Real. Do it as F3 if the whole screen should shift, as a pre-pass multiply if only the game should. |
| F3 | **Night light** (blue-light reduction) | Late-session comfort. | Already implemented upstream in colour management (`nightmode_t`); needs a config field, a Switch + amount slider, and a startup apply. No shader work. | S | Zero extra (the LUT is sampled anyway when colour management is on). | Real, general players. The cheapest win on this list. |
| F4 | **3D LUT (.cube) loader** | Load a community "competitive" or cinematic LUT; per-profile. | The loader, the look slot and the console command exist; missing: `look_path` in `GamescopeSettings`, a Text row (a file path), startup apply, "file missing" reason. Applies to the whole output including the HUD -- acceptable for a look. | S | Zero extra when colour management is already on (it is, for SDR-on-HDR and looks); the 3D LUT is always sampled in the output shader. | Real, high value per line. Note: takes the colour-management path, not the pre-pass, so it also works in HDR. |
| F5 | **Deband + dither** | Removes banding in dark skies/fog; general players notice, shooters rarely. | Neighbourhood effect: 4-8 taps at a random-ish radius plus blue-noise dither, before Pre-Sharpen. | M | Moderate: extra bandwidth at source res, ~0.2-0.5 ms at 1440p on a mid GPU is a guess to verify. | Real for general players; low priority for this user. |
| F6 | **Local contrast / clarity** | "Pop" without the ringing of sharpening; helps muddy games. | Luma unsharp mask at a larger radius (5x5 or separable), applied after `grade()`; RCAS already is the small-radius version. | M | Moderate (same class as F5). | Real, but overlaps Pre-Sharpen; ship only if it reads distinctly different in a screenshot A/B. |
| F7 | **Colour-blindness correction** (Daltonisation, protan/deutan/tritan + strength) | Makes enemy outlines and map markers separable for affected players. | A 3x3 matrix per type in `grade()`, host-selected; 1 Choice + 1 Slider. | S | Negligible. | Real for those who need it, not a gimmick; a "simulate" mode is a gimmick, skip it. |
| F8 | **Luma-only Pre-Sharpen** | Sharpen without colour fringing on red/green edges. | Run RCAS on luma, reattach chroma; a flag bit on the existing sharpen. | S | Negligible. | Real refinement, small. |
| F9 | **Film grain** | Cinematic feel; hides banding a little. | Hash noise in `grade()`, strength + size. | S | Negligible. | Gimmick for this user; cheap enough to keep in a "Look" group if F5 ships. |
| F10 | **Chromatic-aberration removal** | Undo a game's CA when it has no toggle. | Per-channel radial resample (3 taps with a radial offset). | M | Low-moderate. | Mostly a gimmick: nearly every game has a CA toggle, and guessing the game's own CA profile is fragile. |
| F11 | **Vignette removal** | Undo a game's edge darkening. | Radial gain from a guessed falloff. | S | Negligible. | Gimmick; the guess is usually wrong. Skip. |
| F12 | **Sharpen mask that skips a region** | Stop the in-game HUD text from ringing. | A rect (4 params) that zeroes `u_rcasCon` inside it. The compositor's *own* HUD is never sharpened already (it composites after the pass). | S | Negligible. | Marginal; only matters at high Pre-Sharpen. Do after F8, if at all. |
| F13 | **"Dark maps" preset** | One press for Shadow Control + Levels + Vibrancy + Pre-Sharpen values that work. | Not a filter: a *preset* (see X4) applied to the `reshade` section. | S once X4 exists | -- | Real; this is what the user would actually press. |
| F14 | **Per-game auto-enable** | Effects on for Rust, off for the desktop-ish games. | Already solved by the profile model: assign a profile. | 0 | -- | Nothing to build after profiles-concept.md. |
| F15 | **Effects in every capture** | Screenshots and PipeWire streams look like the screen. | `vulkan_screenshot()` (types 1/2, Steam F12, PipeWire) never runs the pass; types 3/4 do (`shader-effects.md` "Follow-up (not done)"). Call the same pre-pass helper there. | S-M | One extra pass per captured frame. | Real gap found in code; changes what a stream carries, so it is an explicit decision. |
| F16 | **HDR-capable effects** | Effects while playing in HDR. | DECISIONS #15 made the pass SDR-only; a linear/PQ-aware `grade()` is a redesign. | L | -- | Not now. Say so in the panel (it already greys with `kSdrOnly`). |

Recommended order: F3, F4, F1, F7, F8, F15, then F5/F6 as a "Look" group if wanted.
F2 folds into F3. F9-F12 only on request.

## Features

| # | Feature | Value | Effort | Concept |
|---|---|---|---|---|
| X1 | **Profiles model + `--profile`** | Removes nine concepts, gives launch-option profiles and per-game everything. | M (mostly deletion) | New model; see profiles-concept.md. |
| X2 | **Keybinds table** (chord -> registry entry or ConCommand) | Toggle an effect, the crosshair, the HUD, focus mode, or switch profile without opening the Shell -- the single most useful thing for a competitive player. | M | New concept (bindings, `keybinds` in `global.json` -- process-level like `overlay`). Rides on `wlserver_process_hotkeys()` (today two hardcoded chords), registry ids (`overlay_e2_set`), and `ritz_profile`. |
| X3 | **Focus mode** (hide HUD + mute toasts, one toggle) | Clean screen for a match. | S | Existing: `cc_toggle_fps_display` (`FpsDisplay.cpp:246`) + `notifications.muted`; a ConCommand that flips both, then a keybind (X2). |
| X4 | **Section presets** (named values for one section: crosshair, filters) | "Crosshair only" profiles without layering; F13. | S-M | New small concept (a preset list per section), copy-once, no routing. Crosshair first. |
| X5 | **Reset this area to defaults** | Safety net once the profile backup is gone; every Param already has `.Default()`. | S | Existing (registry defaults); one Action per area. |
| X6 | **FPS limit while the cursor is free** (menus) | Stop burning 300 fps in menus, full rate in play; gamescope knows the pointer-lock state (cursor-pipeline.md) so "cursor free" is a usable menu proxy. | S-M | Existing: `fps_limit` via `GAMESCOPE_FPS_LIMIT`; a second field + the lock-state hook. |
| X7 | **Screenshot hotkey with effects** | F12-style capture that looks like the screen, to a fixed folder with a timestamp. | S | Existing: `cc_screenshot` type 3 (`steamcompmgr.cpp:1300`) + X2 for the key; F15 for the other types. |
| X8 | **Lag-spike toast / counter** | Know a hitch happened without a graph. | S | Existing detector (`IsSpikeActive`, `FpsDisplay.cpp:372-405`). A toast per spike is noise; a rate-limited count in a Facts row and a session summary toast on quit are the honest shapes. |
| X9 | **Game id from the focused window** (per-window rule) | Non-Steam and persistent-session launches get per-game profiles; DECISIONS #21's known gap (`get_appid_from_pid` exists, is not wired to config). | M | Existing scrape + `SetSessionProfile()` from X1. Mid-session switch was rejected in #21 as confusing; under X1 it is the same pointer change the picker does, with a toast. Rules by window class/title beyond app id: not needed. |
| X10 | **HDR / colour status row** | See at a glance: game output SDR/scRGB/PQ, display HDR on/off, effects active or skipped (SDR-only), look loaded. | S | Existing: `g_eLastBaseLayerColorspace`, `IsBaseLayerSdr()`, `cv_hdr_enabled`, `g_ColorMgmtLooks`. One Facts row in Display. |
| X11 | **Gamma / brightness hotkeys** | Nudge Levels (F1) during play. | S after F1 + X2 | Existing once both land (a bind to `overlay_e2_set` with a delta). |
| X12 | **Config import / export** | Share a profile. | S | Existing: a profile *is* one JSON file; add "profiles folder" to the palette and an "Import from path" Text + Action. Nothing more. |
| X13 | **CLI for any setting** | Script settings from outside. | S | Existing: `gamescopectl overlay_e2_set <id> <n>` / `overlay_e2_get`. Gaps: Choice takes an index, not a name (`crosshair.md`: `hide_mode 0/1/2`); no `overlay_e2_list`; Text rows unreachable; `ritz_profile <name>` missing until X1. Fix the first two. An *offline* editor is unnecessary: the files are plain JSON. |
| X14 | **Steam Deck / embedded parity check** | Know what does not work under DRM. | S (a matrix doc + one laptop/DRM run) | Findings from code: hotkeys go through `wlserver_process_hotkeys()` on every backend, so they work -- but the Deck has no keyboard, so X2 needs Steam Input or a controller path; clipboard sync is nested-only by design; effects defeat direct scanout (battery cost on Deck, worth a note in the panel); Window size request is nested-only and says so. |
| X15 | **Controller shortcuts for the Shell** | Navigate the Shell from a pad. | L | New concept: gamescope has no gamepad input path (games read evdev; the SDL backend does not forward pads). ImGui's gamepad nav exists but nothing feeds it. Steam Input's keyboard emulation covers the Deck case for X2. Not worth it for one desktop user. |
| X16 | **First-run tour** | Onboarding. | M | Existing startup toast (`startup_announce_enabled`). For one user: a **Help / Keys** area listing chords (S) beats a tour. |
| X17 | **Crosshair per game** | Different reticle per game. | 0 after X1 | Existing section; per-game is the profile. Presets (X4) for quick swaps. |

## Top 5 I would build next, in order

1. **X1 Profiles model with `--profile`** -- everything per-game, and the launch-option
   request, rides on it; it also deletes more code than it adds.
2. **X2 Keybinds** -- one table turns X3, X7, X11 and "switch profile" into
   configuration instead of features, and it is what a shooter player reaches for.
3. **F3 + F4 + F1 as a "Colour" group** -- night light and `.cube` looks are already in
   the binary with no row, and Levels is three lines of shader; together they are the
   largest visible change per hour on this list.
4. **X4 Section presets + X5 Reset to defaults** -- the honest answer to "crosshair
   only" profiles and "dark maps", and the safety net that replaces the profile backup.
5. **X6 FPS limit while the cursor is free** -- small, grounded on state gamescope
   already tracks, and the one power/heat feature a competitive player would leave on.
