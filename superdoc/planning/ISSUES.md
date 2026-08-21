# GitHub Issue Breakdown — gamescope-ritz Overlay

This is a draft set of GitHub issues derived from
[`SPEC.md`](./SPEC.md) (the master design spec) and
[`DECISIONS.md`](./DECISIONS.md) (the settled decisions behind it). It turns
the spec's 11-milestone build order (M0–M9b) into issues a developer could
pick up and work, in dependency order. **Nothing here has been posted to
GitHub** — this is a review draft; each block below is written so it can be
pasted into a GitHub issue as-is once approved.

Two setup issues precede the milestones because the spec's Architecture
section names vendoring work (ImGui, `nlohmann::json`) that isn't itself a
milestone but blocks the first two that are. M2 (input capture) and M8
(visual polish) are split into multiple issues — see the note at the top of
each, and the summary below. Every `path:symbol` cited was grep-verified
against this repo's `master` (`fcc1341`) while writing this file. Sizes
(S/M/L) are carried over from `SPEC.md`'s own estimates, splitting a
milestone's size across its child issues where applicable.

**Label set** (applied consistently, multiple per issue where relevant):
`feature`, `setup`, `config`, `vulkan`, `input`, `ui`, `shaders`, `audio`,
`risk`, `deferred`.

---

## Issue 1: Vendor `nlohmann::json` via the subprojects wrap pattern

**Body:** The config system (Issue 3 / M0) needs a JSON library and none is
vendored today. The repo has a live, exact-match precedent for vendoring a
library with no native Meson support: `subprojects/stb.wrap` +
`subprojects/packagefiles/stb/meson.build`, consumed via
`subproject('stb')` at `meson.build:55` (`glm` is the same pattern via
`subproject('glm')` at `meson.build:53`). Add `subprojects/nlohmann_json.wrap`
pinned to a released tag, plus (if the upstream release doesn't already ship
a working `meson.build`) a `packagefiles/nlohmann_json/meson.build`
exposing the header-only target, and wire it into the top-level
`meson.build` the same way `stb`/`glm` are wired in. See `SPEC.md`
Architecture ("Vendoring follows the repo's own established pattern...")
and Build order M0.

**Acceptance criteria:**
- [ ] `subprojects/nlohmann_json.wrap` exists, pinned to a specific tag (not a floating branch).
- [ ] `meson.build` gains a `subproject('nlohmann_json')` call (or equivalent) following the `stb`/`glm` precedent.
- [ ] A throwaway test translation unit that `#include <nlohmann/json.hpp>`, parses a small literal, and serializes it back out compiles and links as part of the normal `meson compile` build.
- [ ] No runtime/vkcube check needed — this issue has no behavior of its own yet.

**Dependencies:** None.

**Size:** S

**Labels:** `setup`, `config`

---

## Issue 2: Vendor ImGui via the subprojects wrap pattern

**Body:** The ImGui render shell (Issue 4 / M1) needs ImGui vendored before
any overlay code can be written. Same precedent as Issue 1
(`subprojects/stb.wrap` + `packagefiles/stb/meson.build`). Add
`subprojects/imgui.wrap` pinned to a tag, with a hand-written
`packagefiles/imgui/meson.build` exposing a `static_library` for
`imgui.cpp`, `imgui_draw.cpp`, `imgui_widgets.cpp`, `imgui_tables.cpp`, and
`backends/imgui_impl_vulkan.cpp`. **Use stock upstream ImGui, not the
`docking` branch** — multi-viewport needs the platform backend to create
real OS windows (meaningless for gamescope), and docking's tile/snap
behavior doesn't match the design guide's free-floating draggable-window
model, which plain `ImGui::Begin` + `SetNextWindowPos` already provides. See
`SPEC.md` Architecture, last paragraph, and Build order M1.

**Acceptance criteria:**
- [ ] `subprojects/imgui.wrap` exists, pinned to a specific stock (non-docking) tag.
- [ ] `packagefiles/imgui/meson.build` builds a static library covering the five files listed above.
- [ ] `meson.build` gains a `subproject('imgui')` call and gamescope-ritz links against it.
- [ ] `meson compile` succeeds with the new dependency linked in, even though nothing calls into ImGui yet (a temporary `IMGUI_CHECKVERSION()` call or equivalent, removed before Issue 4 lands, is an acceptable way to prove symbols resolve).

**Dependencies:** None.

**Size:** S

**Labels:** `setup`, `vulkan`

---

## Issue 3: Build the config system foundation (M0)

**Body:** JSON load/save, schema v1, atomic writes, migration scaffolding,
and app-id resolution — the layer every other feature's persistence plugs
into. No UI yet; drive it with a temporary CLI flag or `gamescopectl` debug
command that dumps the resolved effective config to stderr. Covers: the
global/profile/per-game file layout at `~/.config/gamescope-ritz/`
(`SPEC.md` § Config schema); the five-step app-id resolution order
(`GS_RITZ_APPID` → `STEAM_COMPAT_APP_ID` → nonzero `SteamAppId` →
`STEAM_COMPAT_DATA_PATH` basename → none), read once at the top of
`main()` (`src/main.cpp:723`), before `wlserver_init()`; debounced/batched
disk writes via temp-file-then-`rename()`, never inline on the steamcompmgr
thread; malformed-JSON fallback behavior; profile-name sanitization
(`[A-Za-z0-9 _-]`, no `.`/`..`). Full detail in `SPEC.md` § "Per-feature
sections → 6. Config system" and § "Config schema", plus
[`config-system.md`](./config-system.md) and
[`appid-detection.md`](./appid-detection.md) for background. Buildable and
testable in complete isolation from the overlay.

**Acceptance criteria (vkcube):**
- [ ] Hand-write a `global.json` with `gamescope.filter: "FSR"`, `gamescope.sharpness: 12`; launch `gamescope-ritz -w 640 -h 480 -W 1920 -H 1080 -- vkcube`; the debug dump shows the resolved values, and gamescope visibly applies FSR upscaling at startup.
- [ ] Set `GS_RITZ_APPID=1`, hand-write `games/1.json` with `override_global: true` and a different filter; relaunch with the same env var and confirm the per-game value wins; unset it and confirm `global.json`'s value returns.
- [ ] A deliberately malformed `global.json` fails to parse without crashing gamescope, falls back to hardcoded defaults, and surfaces a visible warning (log line acceptable pre-UI).
- [ ] Schema migration scaffolding exists (`migrate_1_to_2`-shaped chain) even if empty at v1.

**Dependencies:** Vendor `nlohmann::json` via the subprojects wrap pattern.

**Size:** M

**Labels:** `feature`, `config`

---

## Issue 4: Add the ImGui render shell to the composite path (M1)

**Body:** Proves the render path — general-queue command buffer/pool,
descriptor pool, offscreen render target, `Layer_t` injection into
`paint_all()`, fade in/out, and the toggle hotkey/`ConVar` — in isolation
from the harder input-capture problem (Issue 6). Layer injection follows
the existing "HACK HACK HACK" blank-texture layer precedent in `paint_all()`
(`src/steamcompmgr.cpp:2792`-`~2820`); the layer-order convention already
reserves a slot pattern ending in "Primary Overlay (Steam Overlay)" /
"Cursor" (`src/rendervulkan.hpp:26`-`38`, `k_nMaxLayers` is 6,
`src/rendervulkan.hpp:40`), and `nReservedLayers` budgeting
(`src/steamcompmgr.cpp:2718`-`2725`) already guards overlay/cursor layers
from starvation — extend that pattern for the new slot. Use
`generalQueue()`/`generalQueueFamily()`/`generalCommandPool()`
(`src/rendervulkan.hpp:840`,`844`,`842`) — the only queue family with
`VK_QUEUE_GRAPHICS_BIT`, required by ImGui's Vulkan backend, currently
unused by anything (`grep` for these outside `rendervulkan.{cpp,hpp}` is
empty). Wire the toggle via the `forbidden_key` early-return pattern in
`wlserver_handle_key` (`src/wlserver.cpp:319`-`323`), pairing a
`ConCommand` (`cc_focus_info`, `src/steamcompmgr.cpp:932`, is the template
for a callback that just flips an atomic on the owning thread). **First
acceptance criterion is a spike, not an assumption**: the
`VK_KHR_dynamic_rendering` extension-string question (`SPEC.md` Architecture
§ Vulkan plumbing, and Risks #4) — gamescope requests the Vulkan 1.3 feature
bit (`src/rendervulkan.cpp:621`) but never adds the extension string to
`enabledExtensions`, and ImGui's backend docs say the string is required
"even for Vulkan 1.3." Resolve this empirically in the first few minutes of
this issue, before building the rest of the render shell on top of an
unverified assumption. Cross-queue synchronization via `CTimeline`
(`src/Timeline.h:25`, `ToVkSemaphore()` at `:48`) between the general-queue
ImGui draw and the compute-queue composite has no same-frame precedent in
this codebase (Risks #2) — a longer soak test (overlay toggling on a timer
against validation layers) is the real retiring test before this issue is
called done, not just the acceptance run below.

**Acceptance criteria (vkcube):**
- [ ] Spike first: attempt ImGui Vulkan-backend init against gamescope's device creation as-is; if init fails on the missing extension string, add `VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME` to `enabledExtensions` in `src/rendervulkan.cpp`'s device creation and retry. Record which branch was needed.
- [ ] `gamescope-ritz -- vkcube`; press the toggle hotkey; `ImGui::ShowDemoWindow()` (placeholder, not real UI) fades in over the spinning cube.
- [ ] Press again; the demo window fades out.
- [ ] vkcube keeps rendering underneath throughout — proves the layer composites correctly and doesn't stall the vblank loop.
- [ ] Soak test: toggle the overlay on a timer for an extended run against a driver with validation layers enabled; no validation errors, no hang, no crash.

**Dependencies:** Vendor ImGui via the subprojects wrap pattern.

**Size:** M

**Labels:** `feature`, `vulkan`, `risk`

---

## Issue 5: Spike — prove the input-capture gating pattern (M2, part 1 of 3)

**Body:** Input capture is the highest-uncertainty milestone in the whole
roadmap (`SPEC.md` Risks #1: "genuinely new territory with no adaptable
precedent in this codebase"). This issue is split off from the rest of M2
specifically to retire that risk cheaply, as the Risks section itself
recommends, before Issues 6 and 7 build real keyboard/pointer feeds on top
of an unproven design. Every key/pointer listener in
`src/wlserver.cpp:358`-`396` currently always ends by forwarding to
`wlr_seat_keyboard_notify_key`/the pointer equivalents; there is no
"consume into an in-process UI instead of the seat" branch anywhere today.
Build the minimal version of that branch: gate one listener (keyboard is
enough for this spike) on `g_bSettingsOverlayVisible`, feed the raw keysym
into a bare `ImGuiIO` (structurally similar to how the dead-code
`CLibInputHandler`, `src/LibInputHandler.h:11`, already translates raw
libinput events into `wlserver_key` calls — same translation target class,
different destination), and confirm the game-facing forward call is
genuinely skipped, not just not-yet-observed.

**Acceptance criteria (vkcube):**
- [ ] With the overlay open (Issue 4's demo window), add a temporary debug counter that logs forwarded key events to vkcube's focused window; confirm it stays at zero while the overlay is open.
- [ ] Toggle the overlay closed; confirm forwarding resumes immediately (counter increments again on the next keypress).
- [ ] Document, in the PR/issue, the exact gating point chosen in `wlserver_handle_key` and why it's safe to extend to the pointer handlers in Issue 7.

**Dependencies:** Add the ImGui render shell to the composite path.

**Size:** S

**Labels:** `input`, `risk`

---

## Issue 6: Capture keyboard input into the overlay (M2, part 2 of 3)

**Body:** Full keyboard capture and release, building on the gating pattern
proven in Issue 5. Extend the gate to all of `wlserver_handle_key`'s forward
paths (not just the spike's single case), and build the real `ImGuiIO`
keyboard feed: keysym translation, modifier state, key-repeat. The
genuinely unsolved part per `SPEC.md` Architecture § Input capture and
release is what happens to a game's held-key state when capture cuts it off
mid-press — a key-up the game never receives — and the symmetric case of a
key held before toggle-on and released after toggle-off. Budget real testing
time here; there is no crash/edge-case history in this codebase to lean on.

**Acceptance criteria (vkcube):**
- [ ] With the overlay open, type into a text-input widget in the demo/real UI (e.g. `ImGui::ShowDemoWindow()`'s input fields) and confirm characters land in ImGui, not vkcube's window.
- [ ] The zero-forwarded-events debug counter (from Issue 5, extended to cover all key codes/modifiers) stays at zero for the full range of keys exercised, and resumes immediately on close.
- [ ] Hold a key down, toggle the overlay open then closed while still holding it, release it, and confirm no stuck-key state — check via the debug counter or vkcube's own lack of hang/crash as a coarse signal.
- [ ] Hold a key down *before* opening the overlay, toggle open, and confirm the in-progress press doesn't leak a spurious key-down into ImGui.

**Dependencies:** Spike — prove the input-capture gating pattern.

**Size:** M

**Labels:** `input`, `risk`

---

## Issue 7: Capture pointer input into the overlay (M2, part 3 of 3)

**Body:** Full pointer (mouse motion/button/axis) capture and release,
using the same gating pattern as Issue 6 but applied to the pointer
listeners in `src/wlserver.cpp:358`-`396` instead of the keyboard listener.
Split from Issue 6 because it touches a distinct set of handler functions
with its own edge cases (drag state, button-held-across-toggle) and can be
picked up by a different person in parallel once Issue 5's spike has proven
the shared design — the two do not need to be sequential relative to each
other, only both after the spike.

**Acceptance criteria (vkcube):**
- [ ] With the overlay open (Issue 4's demo window), drag the demo window by its titlebar and click its buttons — confirms the overlay owns the pointer.
- [ ] A zero-forwarded-events debug counter for pointer motion/button/axis events (parallel to Issue 6's keyboard counter) stays at zero while the overlay is open, resumes immediately on close.
- [ ] Hold a mouse button down, toggle the overlay open then closed while still holding it, release it, and confirm no stuck-button state.

**Dependencies:** Spike — prove the input-capture gating pattern.

**Size:** M

**Labels:** `input`, `risk`

---

## Issue 8: Build the live gamescope options tab (M3)

**Body:** Filter/scaler segmented controls, the auto-corrected sharpness
slider, and VRR/HDR/tearing toggles. This is the cheapest feature in the
whole roadmap and validates the full input→ImGui→X11-property→live-effect
loop before the harder features build on top of it — it should be the first
real panel built once input capture works. Almost no new plumbing: every
value already has a `g_wanted*` variable an external writer sets and a
plain `g_*` variable `paint_all()` reads each frame
(`src/steamcompmgr.cpp:9330`-`9331`); write through the existing X11
root-window properties — `GAMESCOPE_NEW_SCALING_FILTER`/
`GAMESCOPE_NEW_SCALING_SCALER` (atoms at `src/steamcompmgr.cpp:8275`-`8276`)
and `GAMESCOPE_FSR_SHARPNESS`/`GAMESCOPE_SHARPNESS` (atoms at `:8242`-
`:8243`) — the same mechanism the Steam client itself already uses, not a
new code path. VRR/HDR/tearing map to real `ConVar`s
(`cv_adaptive_sync`, `cv_hdr_enabled`, `cv_tearing_enabled`,
`src/steamcompmgr.cpp:295`,`461`,`455`). **The sharpness slider is one
auto-corrected control, not a raw passthrough** (`DECISIONS.md` #9): FSR
maps `raw/10.0` (higher raw = sharper, `src/rendervulkan.cpp:4109`), NIS
maps `(20-raw)/20.0` (higher raw = less sharp, `:4123`) — invert directions.
The UI must present one "sharper →" direction regardless of active filter,
remapping to the correct raw value on write. Surface the hardcoded
FIT/LINEAR override that applies while the Steam window is focused
(`src/steamcompmgr.cpp:9320`-`9325`) rather than showing a lying "Filter:
FSR" while it's active. Full detail: `SPEC.md` § "Per-feature sections → 4.
Live gamescope options".

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -w 640 -h 480 -W 1920 -H 1080 --backend sdl -- vkcube`; open the overlay, switch Filter to FSR, drag the sharpness slider and visually confirm the upscaled cube sharpens.
- [ ] Switch Filter to NIS and confirm the slider's displayed direction still reads "sharper →" despite the underlying raw value now needing to move the opposite way — proves the remap, not just the write.
- [ ] Toggle VRR/HDR/tearing and confirm each writes through to its `ConVar` (observable via `gamescopectl` or a log line).

**Dependencies:** Capture keyboard input into the overlay; Capture pointer input into the overlay.

**Size:** S

**Labels:** `feature`, `ui`, `vulkan`

---

## Issue 9: Add the always-on FPS display (M4)

**Body:** An always-drawn frame-rate readout, independent of the settings
panel's open/closed state, with configurable font size, backdrop, and blend
mode. Needs a second, independently-gated ImGui draw call in the same
`paint_all()`-adjacent block as the settings panel, but on its own
visibility flag — it must render every composited frame regardless of the
settings panel's state. **Reuse mangoapp's math, not its rendering**
(`DECISIONS.md` #11, #12): the headline number is the game's own frame
rate, `frametime_ns = lastCommit->present_time -
w->last_commit_present_time` (`src/steamcompmgr.cpp:7466`, inside
`handle_presented_for_window`, already on the steamcompmgr thread) — the
same computation mangoapp's `app_frametime_ns` field mirrors. Read this
value directly in-process; do not round-trip through the one-shot
`request_app_performance_stats` Wayland protocol
(`protocol/gamescope-control.xml:135`-`143`), which is for external clients.
mangoapp's own HUD rendering is a fully separate external process gamescope
has zero rendering control over (no reachable font/backdrop/blend), which is
why the rendering here is native ImGui, not a reused mangoapp surface. Full
settings table: `SPEC.md` § "Per-feature sections → 3. FPS display".

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; the FPS counter is visible in the corner with the settings panel closed, showing a plausible number close to vkcube's own reported frame rate.
- [ ] Toggle the settings panel open and closed; the FPS readout's own visibility is unaffected either way.

**Dependencies:** Add the ImGui render shell to the composite path.

**Size:** S

**Labels:** `feature`, `ui`

---

## Issue 10: Add PipeWire volume control to the overlay (M5)

**Body:** Shows and controls the system-level PipeWire volume of the hosted
game's process (a Stream node, not the whole-output Sink/Device). **v1
shells out to `wpctl`** (`DECISIONS.md` #17) rather than embedding a second
`pw_core` connection — `wpctl set-volume ID VOL[%] --pid` / `wpctl
set-mute ID 1|0|toggle --pid` already implements "given a PID, find and
control its node(s)". The hard part is finding the PID(s): `wpctl --pid`
matches one exact PID, not a process tree; use
`Process::GetChildPids(pid_t)` (`src/Utils/Process.cpp:65`, walks
`/proc/*/stat` parent links) starting from the PID `GamescopeReaperProcess`
wraps (`src/Apps/gamescopereaper.cpp`), recurse to the full descendant set,
and try each against `wpctl status`/`pw-dump` output for a
`Stream/Output/Audio` node whose `application.process.id` matches. **The
control hides itself when no node is found** (`DECISIONS.md` #18) — no
picker, show "audio: not detected" and keep polling; no gamescope-side
volume persistence (WirePlumber's own `node.stream.restore-props` already
owns that). Curve: `channelVolumes` is raw linear amplitude; present a cubic
display curve (`linear = (display_fraction)^3`), 0–100% with optional 150%
boost. **Top open risk for this feature**: Steam Linux Runtime's
`pressure-vessel`/`bwrap` sandboxing can put the real Proton process in a
PID namespace gamescope's own `/proc` walk never sees, silently breaking PID
matching — no scout could verify this during planning (no game was
launched); this is the concrete moment to retire that risk, not a separate
task. Full detail: `SPEC.md` § "Per-feature sections → 5. PipeWire volume",
[`pipewire-loudness.md`](./pipewire-loudness.md).

**Acceptance criteria (vkcube):**
- [ ] vkcube itself is silent, so pair it with an audio-producing sibling: `gamescope-ritz -- bash -c 'vkcube & speaker-test -t sine -f 440 -c 1; wait'`. Open the overlay's Audio panel; it detects the `speaker-test` stream.
- [ ] Drag the volume slider; a separate terminal's `wpctl status`/`pw-dump` shows the corresponding node's volume changing.
- [ ] Kill `speaker-test`; the panel falls back to "not detected" without erroring.
- [ ] Run once against a real Steam/Proton title if available during implementation, to confirm or refute the `bwrap` PID-namespace risk directly; record the result either way even if the fallback UI is what's observed.

**Dependencies:** Capture keyboard input into the overlay; Capture pointer input into the overlay.

**Size:** M

**Labels:** `feature`, `audio`, `risk`

---

## Issue 11: Build the combined ReShade effect (Vibrancy + Pre-Sharpen) and Shaders panel (M6)

**Body:** The real `gamescope-ritz.fx` plus the Shaders panel UI, kept as
one issue rather than split because the uniform names in the `.fx` file and
the UI controls that drive them must stay in lockstep — splitting shader
authorship from the panel that consumes it would fragment one train of
thought for no parallelism gain (both need the same person's judgment on
naming and pass ordering). `vulkan_composite()` runs ReShade first
(`src/rendervulkan.cpp:4037`-`4059`, confirmed:
`g_reshadeManager.pipeline(key)` call at `:4051`), only on
`frameInfo->layers.get(0)`, before FSR/NIS scaling and before HDR tonemap.
**Every parameter must be a `source`-tagged uniform** — such a uniform lives
in a mutex-guarded map (`g_runtimeUniforms`/`g_runtimeUniformsMutex`,
`src/reshade_effect_manager.cpp:30`-`31`), updated every frame via
`RuntimeUniform::update()` (`:541`) and written via
`reshade_effect_manager_set_uniform_variable()` (`:1938`) — a plain
`memcpy`, no recompile. A uniform without `source` falls through to
`DataUniform` (`:196`) and is inert to any UI control. **Ship as one
always-loaded technique, sequential gated passes** (`SPEC.md` § "Per-feature
sections → 2", the load-bearing reason preserved there): any change to the
active `ReshadeEffectKey` — including a technique switch — forces a full
synchronous FX parse + SPIR-V codegen + Vulkan pipeline build inline on the
steamcompmgr thread (`ReshadeEffectManager::pipeline()`, confirmed via
`m_lastKey` cache-miss path). Gating each effect behind its own on/off
uniform (0 = no-op) inside one technique means toggling costs a `memcpy`,
not a recompile. **Build the pass ordering and uniform namespacing so
Adaptive Brightness (Issue 18) can be appended later without touching
Vibrancy/Sharpness or their uniform names.** Label the two sharpness paths
distinctly in the UI: "Upscale Sharpness" (Issue 8's post-upscale control)
vs. "Pre-Sharpen" (this pass, pre-upscale, works regardless of filter).
**HDR gate**: when the focused layer's colorspace isn't `LINEAR`/`SRGB`
(HDR active), the Shaders panel controls grey out with an explanation
("Effects are SDR-only...") using the design guide's existing
disabled-control treatment — do not apply effects whose math assumes
clamped SDR RGB. Settings table and full reasoning: `SPEC.md` § "Per-feature
sections → 2. ReShade effects", [`reshade-shaders.md`](./reshade-shaders.md).

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the Shaders panel; toggle Vibrancy on and drag its strength slider, visually confirm the cube's color saturation changes.
- [ ] Toggle Pre-Sharpen with Filter set to `LINEAR` (where Upscale Sharpness is a documented no-op) and confirm it still visibly sharpens — the one case that justifies keeping both sharpness paths.
- [ ] Toggle each effect on/off repeatedly and confirm no recompile stutter on any toggle (only a uniform write) — the empirical proof the combined-`.fx` design achieves what it was chosen for.
- [ ] Force HDR on (`gamescope.hdr_enabled`) and confirm the Shaders panel greys out with the SDR-only explanation rather than applying visibly wrong color math.

**Dependencies:** Capture keyboard input into the overlay; Capture pointer input into the overlay.

**Size:** L

**Labels:** `feature`, `shaders`, `ui`

---

## Issue 12: Wire full config persistence into the overlay panels (M7)

**Body:** Connects Issue 3's config system to Issues 8/9/11's live panels so
edits actually persist, plus the "Override Global Config" full-snapshot
checkbox, one-time-copy profile apply, and the Config/Profiles panel UI
itself (not designed in the source mockup — build fresh using the flat/
hairline language, `SPEC.md` § "UI structure"). **"Override Global Config"
takes a full snapshot, not a diff** (`DECISIONS.md` #14): flipping the
checkbox writes `games/<AppId>.json` containing the game's fully resolved
effective settings at that instant, not a sparse delta; resolution becomes
strictly two-level (`games/<AppId>.json` if `override_global: true`, or
`global.json` — never both merged key-by-key). **Applying a profile copies
values once** (`DECISIONS.md` #15): editing a profile later does not
retroactively affect games that already applied it. Where a setting already
maps to a real `ConVar` (VRR/HDR/tearing), write via `cv.SetValue()`; where
it's a plain global (filter/scaler/sharpness), write through the existing
X11-property mechanism from Issue 8, not by reaching into `g_wanted*`
directly. Disk I/O must never happen inline on the steamcompmgr thread
(queue the write, flush from the otherwise-idle main thread or a one-shot
worker) — per Issue 3's debounce design. Full detail: `SPEC.md` §
"Per-feature sections → 6" and § "Config schema".

**Acceptance criteria (vkcube):**
- [ ] With `GS_RITZ_APPID=1` set, `gamescope-ritz -- vkcube`; change the filter in the Gamescope panel, enable "Override Global Config"; `games/1.json` is written containing a full snapshot (not just the filter key).
- [ ] Create a profile, apply it; the per-game file's values update to match.
- [ ] Edit the profile file directly on disk and relaunch; the game's already-applied values do **not** change — proves one-time-copy, not live reference.

**Dependencies:** Build the config system foundation; Capture keyboard input into the overlay; Capture pointer input into the overlay; Build the live gamescope options tab.

**Size:** M

**Labels:** `feature`, `config`, `ui`

---

## Issue 13: Build the IBM Plex typography system (M8, part 1 of 4)

**Body:** First of four M8 sub-issues. M8 is split because "nearly every
control in this design deviates from stock ImGui's default rendering"
(`SPEC.md` Build order M8) covers genuinely separable work: typography,
widget geometry, window/dock chrome, and icon authoring can each be picked
up by a different person once the functional panels (Issue 12) exist. This
issue is sequenced first among the four because widget labels (Issue 14)
and chrome titles (Issue 15) both render text and should build on settled
font metrics rather than guess at them. Covers the IBM Plex Sans/Mono font
atlas (Plex Mono with tabular figures for all numeric readouts, per
`ui-design-guide.md`) and the tracked-text letter-spacing helper the design
guide's typography calls for.

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the overlay; body/label text renders in IBM Plex Sans.
- [ ] Numeric readouts (FPS counter, sharpness value, volume percentage) render in IBM Plex Mono with tabular (fixed-width digit) alignment — confirm digits don't shift horizontally as values change.
- [ ] A sample tracked-text label visually matches the design guide's letter-spacing example.
- [ ] No functional regression versus Issue 12's behavior.

**Dependencies:** Wire full config persistence into the overlay panels.

**Size:** S

**Labels:** `ui`

---

## Issue 14: Build custom ImDrawList widget rendering (M8, part 2 of 4)

**Body:** Second of four M8 sub-issues (see Issue 13 for the split
rationale). Custom `ImDrawList` draw code replacing stock ImGui's rounded
default widgets with the design guide's rectangular slider handles, square
toggle knobs, and filled-square checkboxes, across every slider/toggle/
checkbox introduced by Issues 8, 9, 10, 11, and 12.

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the overlay; sliders, toggles, and checkboxes across all panels render with rectangular/square geometry, not stock ImGui rounded widgets.
- [ ] Exercise one control of each type (drag a slider, click a toggle, click a checkbox) and confirm identical functional behavior to before this issue — no regression in the panels built by Issues 8, 9, 10, 11, 12.

**Dependencies:** Build the IBM Plex typography system.

**Size:** L

**Labels:** `ui`

---

## Issue 15: Build window and dock chrome (M8, part 3 of 4)

**Body:** Third of four M8 sub-issues (see Issue 13 for the split
rationale). Window chrome (accent border/glow on focus, status dots), dock
chrome (per design guide: centered, 38px from bottom edge, 54×54px square
icon buttons, 5px gaps), and the flat semi-opaque panel fill used in place
of true backdrop blur (`DECISIONS.md` #5 — blur was dropped in favor of
fade in/out plus flat translucency, since ImGui has no native blur
primitive).

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the overlay; the focused window shows the accent border/glow treatment; panels render as flat, near-black translucent rectangles (no blur-behind).
- [ ] The dock renders centered, 38px from the bottom edge, with 54×54px square buttons and 5px gaps, matching the design guide.
- [ ] Fade in/out on toggle (from Issue 4) still works unchanged.

**Dependencies:** Build the IBM Plex typography system.

**Size:** M

**Labels:** `ui`

---

## Issue 16: Author the overlay icon SVG set (M8, part 4 of 4)

**Body:** Fourth of four M8 sub-issues (see Issue 13 for the split
rationale) — this one is pure asset authoring with no functional
dependency on Issues 14/15, so it can proceed fully in parallel with them
once the panel/dock structure from Issue 12 is settled. Author 8–10
geometric line-drawn SVG glyphs (1–1.5px stroke, no rounded corners,
16–20px grid, monochrome, outline-first with small solid-fill accents,
explicitly not copied from the mockup's own placeholders — per
`SPEC.md` § "UI structure", icon list): (1) Settings/gear, (2) Display/
scaling, (3) Shaders/color grading, (4) Performance/FPS, (5) Audio, (6)
Profiles/game-config, (7) Reset/restore (no mockup source — needs a fresh
glyph), (8) Close (×), (9) Collapse/minimize (–), and optionally (10) Dock
overflow/more — only if the fixed dock button count is ever exceeded; may
be dropped if six panel icons plus chrome icons fit without overflow.

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the overlay; all in-scope icons (at minimum 1–9) render in the dock and window chrome at the dock's 54×54px button size, legible and visually consistent with each other.
- [ ] Icon style (stroke weight, grid, monochrome-with-accent) matches the design guide's rules side-by-side.

**Dependencies:** Wire full config persistence into the overlay panels.

**Size:** S

**Labels:** `ui`

---

## Issue 17: Spike — verify persistent-texture behavior for Adaptive Brightness (M9a, deferred)

**Body:** **Deferred and off the critical path** — nothing above depends on
this, and it has no dependency of its own (it is CLI-flag driven, not
overlay/input-driven), so it can genuinely be picked up any time; it is
listed last only because Adaptive Brightness itself ships last, matching
`SPEC.md`'s own ordering. Do not schedule this ahead of Issues 3–16. A
minimal throwaway `.fx`: one pass that increments a value in a persistent
1×1 texture and displays it as a background tint, driven by the existing
`--reshade-effect`/`--reshade-technique-idx` CLI flags — no ImGui needed.
This answers whether `discardImage()` (called at the top of every
`ReshadeEffectPipeline::execute()`, `:1766`-`1776`) actually preserves
render-target content frame-to-frame, or merely sets a layout-transition
hint per the Vulkan spec's implementation-defined guarantees. **No shipped
effect in this repo exercises this path** — confirm empirically before any
real engineering time goes into the full Adaptive Brightness pipeline
(Issue 18). See `SPEC.md` Risks #3 and § "Per-feature sections → 2.
Adaptive Brightness specifically".

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz --reshade-effect <spike.fx> -- vkcube`; the background tint visibly drifts/cycles across frames rather than resetting every frame.
- [ ] Record the result (drifts vs. resets) in the issue/PR regardless of outcome — a "resets" result means Issue 18's design needs rethinking before it starts, which is exactly what this spike exists to catch early.

**Dependencies:** None.

**Size:** S

**Labels:** `shaders`, `risk`, `deferred`

---

## Issue 18: Add Adaptive Brightness to the combined effect (M9b, deferred)

**Body:** **Deferred and off the critical path.** Appends the Adaptive
Brightness passes to the `gamescope-ritz.fx` file Issue 11 already shipped
— the file's pass ordering and uniform namespacing were built in Issue 11
specifically to allow this without restructuring Vibrancy/Pre-Sharpen — plus
its group in the Shaders panel (enabled toggle, target-luminance slider,
up/down adaptation-speed sliders, min/max gain sliders, strength slider).
Settings table (target_luminance 0.1–0.9 default 0.5, adapt_up/down_speed
0.1–5.0 default 1.0, min_gain 0.5–1.0 default 0.5, max_gain 1.0–2.0 default
2.0, strength 0.0–1.0 default 1.0 as the dry/wet mix — use strength to fade
the effect, not the enabled flag, so adaptation state isn't lost) is in
`SPEC.md` § "Per-feature sections → 2" and already reserved in the config
schema. Do not start this issue until Issue 17 has confirmed the
persistent-texture assumption holds.

**Acceptance criteria (vkcube):**
- [ ] `gamescope-ritz -- vkcube`; open the Shaders panel, toggle Adaptive Brightness on; no recompile stutter on toggle — same uniform-gated-pass proof as Issue 11, now for the third pass.
- [ ] Vibrancy/Pre-Sharpen (Issue 11) still work unchanged with Adaptive Brightness enabled alongside them — proves the passes compose rather than conflict.

**Dependencies:** Build the combined ReShade effect (Vibrancy + Pre-Sharpen) and Shaders panel; Spike — verify persistent-texture behavior for Adaptive Brightness.

**Size:** M

**Labels:** `feature`, `shaders`, `deferred`

---

## Suggested order of work

1. Vendor `nlohmann::json` via the subprojects wrap pattern
2. Vendor ImGui via the subprojects wrap pattern
3. Build the config system foundation (M0)
4. Add the ImGui render shell to the composite path (M1)
5. Spike — prove the input-capture gating pattern (M2, part 1 of 3)
6. Capture keyboard input into the overlay (M2, part 2 of 3)
7. Capture pointer input into the overlay (M2, part 3 of 3)
8. Build the live gamescope options tab (M3)
9. Add the always-on FPS display (M4)
10. Add PipeWire volume control to the overlay (M5)
11. Build the combined ReShade effect (Vibrancy + Pre-Sharpen) and Shaders panel (M6)
12. Wire full config persistence into the overlay panels (M7)
13. Build the IBM Plex typography system (M8, part 1 of 4)
14. Build custom ImDrawList widget rendering (M8, part 2 of 4)
15. Build window and dock chrome (M8, part 3 of 4)
16. Author the overlay icon SVG set (M8, part 4 of 4)
17. Spike — verify persistent-texture behavior for Adaptive Brightness (M9a, deferred)
18. Add Adaptive Brightness to the combined effect (M9b, deferred)

Issues 6 and 7 (keyboard/pointer capture) can be worked in parallel once
Issue 5's spike lands. Issues 13–16 (M8's four sub-issues) can likewise be
distributed across multiple people once Issue 12 lands — 16 in particular
has no code dependency on 14/15 and can start immediately in parallel with
either. Issues 17–18 are deferred and should not be pulled ahead of Issue
12 in practice, even though Issue 17 has no hard dependency that would
block it technically.
