# Runtime Knobs & FPS Display — planning notes for the ImGui settings overlay

Scouting doc only. No code. Claims tied to a `path:symbol` are grep/read-verified
against this repo's source as of current `master` (fcc1341). A handful of claims are
external — upstream `ValveSoftware/gamescope` issues/discussion, MangoHud's own docs,
and the Wayland `wp_presentation` protocol spec — each is marked **"External
corroboration"**, links its source with the date checked, and is kept visibly separate
from in-repo verification; treat those as corroborating context, not as claims about
*this* fork's code. Full link list in [External references](#external-references) at
the bottom.

---

## Part A — Live gamescope options

**Verdict: filter and sharpness are already fully live-changeable today, with zero
new plumbing needed.** Gamescope already has a working "external process changes a
setting while gamescope runs" path — it's how the Steam client drives these same
options right now — and the new overlay's job is to call into that same path, not
invent a new one.

### A1/A2 — mechanism, and startup-only vs. live

Every filter/sharpness value has **two backing variables**: a `g_wanted*` value that
external input writes, and a plain `g_*` value that `paint_all()` actually reads each
frame. They're reconciled once per frame on the steamcompmgr thread:

```cpp
// src/steamcompmgr.cpp:9330-9331 (inside the per-frame body, steamcompmgr thread)
g_upscaleScaler = g_wantedUpscaleScaler;
g_upscaleFilter = g_wantedUpscaleFilter;
```

(Exception: while the Steam window itself is focused, these two lines are skipped in
favor of a hardcoded `FIT`/`LINEAR` override — `src/steamcompmgr.cpp:9320-9325`.)

Three independent writers of `g_wanted*`/`g_upscaleFilterSharpness`, **all already
live at runtime, no rebuild/restart needed**:

1. **CLI flags at startup** — `--filter`/`-F` → `parse_upscaler_filter()`
   (`src/main.cpp:409`) → `g_wantedUpscaleFilter` (`src/main.cpp:767`); `--sharpness`
   → `g_upscaleFilterSharpness` (`src/main.cpp:821`). Startup-only *as CLI*, but the
   variables they set are the same ones live-writable below.
2. **X11 root-window properties**, polled by the steamcompmgr thread's own
   `PropertyNotify` handler (`src/steamcompmgr.cpp`, inside the event loop that also
   owns focus/window state):
   - `GAMESCOPE_SCALING_FILTER` (legacy 0-4 enum bundling filter+scaler,
     `src/steamcompmgr.cpp:6547-6572`)
   - `GAMESCOPE_NEW_SCALING_FILTER` (`:6693-6699`) / `GAMESCOPE_NEW_SCALING_SCALER`
     (`:6702-6708`) — direct `GamescopeUpscaleFilter`/`GamescopeUpscaleScaler` enum
     values, one property each, added later (this is the one a new UI should target)
   - `GAMESCOPE_FSR_SHARPNESS` / `GAMESCOPE_SHARPNESS` (`:6576-6580`) →
     `g_upscaleFilterSharpness`, clamped `0..20`
   - Atoms interned at `src/steamcompmgr.cpp:8241-8243,8275-8276`.
3. **`gamescopectl <cvar> <value>`** / any `gamescope_private.execute` client — only
   for values that are actual `ConVar`s (see A5; filter/sharpness are **not** ConVars,
   they're raw globals, so this path doesn't apply to them today — X11 properties are
   the only live-write mechanism for filter/scaler/sharpness specifically).

So: **filter, scaler, and sharpness are read per-frame, not once at init.** They are
already a solved "runtime option" in this codebase; the pattern the new overlay should
copy is #2 (write the X11 property, let the existing steamcompmgr-thread handler pick
it up), not add a third code path.

### A3 — thread safety

The two backing types behave differently:

- `g_upscaleFilter`/`g_upscaleScaler`/`g_wantedUpscaleFilter`/`g_wantedUpscaleScaler`/
  `g_upscaleFilterSharpness` are **plain globals**, not atomics
  (`src/main.hpp:62-66`, `extern GamescopeUpscaleFilter g_upscaleFilter;` etc., no
  `std::atomic`). They are safe **today** only because every writer (X11 property
  handler, CLI parse) runs on the steamcompmgr thread — CLI at startup before the
  second thread exists, the property handler inside `steamcompmgr_main`'s own loop.
  **A raw write from a UI thread other than steamcompmgr would be a data race** against
  `paint_all()`'s per-frame reads.
- `ConVar<T>::m_Value` (`src/convar.h:127`, field around line 138) is **also a plain
  `T`**, not atomic — `operator=`/`SetValue` just assigns
  (`src/convar.h`, `SetValue`). Yet `gamescope_private.execute` already writes
  ConVars from the **main thread** (`gamescope_control_execute` →
  `ConCommand::Exec`, `src/wlserver.cpp:1426-1431`) while the steamcompmgr thread reads
  some of them per-frame with no visible lock at the read site (e.g.
  `frameInfo.allowVRR = cv_adaptive_sync;`, `src/steamcompmgr.cpp:2620`). This is a
  **pre-existing, already-shipped cross-thread pattern** — not something this task
  introduces — but it means "is it actually safe" is inherited risk, not a new
  guarantee (see Risks).
- **Practical guidance for the overlay:** if the ImGui overlay ends up rendering on
  the steamcompmgr thread (piggybacking on `paint_all()`/`vulkan_composite()` each
  frame — the natural place, since that's the thread that already owns the GPU
  submission), writing `g_wanted*`/`g_upscaleFilterSharpness` directly is safe with no
  new synchronization. If it runs elsewhere, the safe options are (a) route writes
  through `XChangeProperty` on the existing `GAMESCOPE_NEW_SCALING_FILTER` /
  `GAMESCOPE_NEW_SCALING_SCALER` / `GAMESCOPE_SHARPNESS` atoms — reusing the
  already-serialized, already-tested path — or (b) accept the same "matches existing
  ConVar risk profile" tradeoff other in-tree writers already accept.

### A4 — full value set

| Knob | Type / range | Default | Applies to | Evidence |
|---|---|---|---|---|
| Filter | `GamescopeUpscaleFilter`: `LINEAR`, `NEAREST`, `FSR`, `NIS`, `PIXEL` | `LINEAR` | all layers | `src/main.hpp:35-42`, default `src/main.cpp:313` |
| Scaler | `GamescopeUpscaleScaler`: `AUTO`, `INTEGER`, `FIT`, `FILL`, `STRETCH` | `AUTO` | content-fit, independent of filter | `src/main.hpp:53-59`, default `src/main.cpp:314` |
| Sharpness | `int`, `0..20`, one shared control | `2` | **only** `FSR` and `NIS`; ignored/no-op for `LINEAR`/`NEAREST`/`PIXEL` | `src/main.hpp:66`, default `src/main.cpp:318`, clamp `src/steamcompmgr.cpp:6578` |

Sharpness is **not** a single unified 0-1 scale — each filter remaps the same raw
`0..20` int into its own native range, and they invert:

- FSR (RCAS): `g_upscaleFilterSharpness / 10.0f` → `0.0..2.0` (`src/rendervulkan.cpp:4109`) — higher raw value = sharper.
- NIS: `(20 - g_upscaleFilterSharpness) / 20.0f` → `1.0..0.0` (`src/rendervulkan.cpp:4123`) — higher raw value = **less** sharp (NIS's native parameter runs the opposite direction). A UI must not present one slider whose meaning silently flips depending on the selected filter without a label change, or must normalize this itself and remap on write.

`PIXEL` self-downgrades to `NEAREST` when the scale ratio is an exact integer on both
axes (`src/steamcompmgr.cpp:2248-2252`); otherwise it currently behaves like whatever
`Layer_t::filter` already was — worth surfacing in UI copy ("Pixel: sharp when the
scale factor is a whole number") rather than promising a distinct look always.

**External corroboration (not this repo's source, upstream `ValveSoftware/gamescope`,
checked 2026-08-21):** [Issue #515 "Make sharpness setting user-friendly"](https://github.com/ValveSoftware/gamescope/issues/515)
confirms this exact opposite-direction inconsistency was independently reported
upstream — "lower value mean more sharpening" for FSR, "higher value mean more
sharpening" for NIS, with the reporter's proposed unified mapping matching the same
three ranges found in this repo's source (BLIT 0-20, FSR `2.0→0.0`, NIS `0.0→1.0`).
The issue was **closed as not planned** — i.e. upstream Valve chose not to normalize
this, so a UI-side fix (relabeling/remapping in the overlay, not touching the shared
`g_upscaleFilterSharpness` value) is the realistic path, matching this doc's earlier
recommendation. The same issue and a GamingOnLinux writeup both describe the
sharpness value as already hotkey-adjustable in-game (`Super+I`/`Super+O`) on Steam
Deck — independent confirmation from outside this codebase that gamescope's own
sharpness/filter values are treated as live, runtime-mutable state in production, not
startup-only, even though the hotkey binding itself lives outside this fork's source
tree (SteamOS's Gamepad UI, not found under `src/` here) and was not traced further.

### A5 — other live-adjustable options worth surfacing

All verified live via the same X11-property-into-steamcompmgr-thread mechanism, or
(marked) via `ConVar` + `gamescope_private`:

| Option | Mechanism | Evidence |
|---|---|---|
| **VRR / adaptive sync** | `ConVar<bool> cv_adaptive_sync` (default `false`, `src/steamcompmgr.cpp:295`), read per-frame at `frameInfo.allowVRR = cv_adaptive_sync` (`:2620`) and `:8465`; also settable via `GAMESCOPE_VRR_ENABLED` X11 property (`:6680-6681`) | live |
| **HDR toggle** | `ConVar<bool> cv_hdr_enabled` (`:461`), recomputed into `g_bOutputHDREnabled` every frame (`:9074`); also settable via `GAMESCOPE_DISPLAY_HDR_ENABLED` property (`:6712-6713`) | live |
| **FPS limiter** | plain `int g_nSteamCompMgrTargetFPS` (`:960`), set live via `GAMESCOPE_FPS_LIMIT` property (`:6618-6620`), read every frame (`:6181`, `:7458`, `:9201`) | live |
| **Tearing** | `ConVar<bool> cv_tearing_enabled` (`:455`) | likely live (ConVar, same class as VRR/HDR) — not traced end-to-end here, flagged as probable not confirmed |
| **Force internal display** | `bool g_bForceInternal`, live via `GAMESCOPE_DISPLAY_FORCE_INTERNAL` property, triggers `GetBackend()->DirtyState()` (`:6683-6684`) | live, DRM-backend-relevant only |

`INTEGER` scaler (pixel-perfect integer scaling) is not a separate toggle from A4's
Scaler dropdown — it's already one of the five `GamescopeUpscaleScaler` values, so no
extra UI element needed beyond the scaler picker itself.

---

## Part B — FPS display

**Verdict: recommend drawing the counter ourselves in ImGui, not reusing mangoapp.**
mangoapp is a data *bridge* to a separate, external, unstyleable process — it gives
gamescope no rendering control at all, which conflicts directly with the font
size/backdrop/blending requirements. But mangoapp's timing computation is exactly the
data source to reuse; only the drawing should be new.

**External corroboration (checked 2026-08-21):** [MangoHud's own `data/MangoHud.conf`](https://github.com/flightlessmango/MangoHud/blob/master/data/MangoHud.conf)
and README confirm mangoapp's *consumer* is a fully independent, richly configurable
renderer with exactly the kind of controls this task wants — but all of them live in
MangoHud's own config file, not anywhere gamescope can reach: `font_size` (default
`24`, with a separate `font_scale`), `background_alpha` ("Hud transparency / alpha",
0.0-1.0), `alpha` (overall HUD transparency), `round_corners` (corner roundness),
`text_outline`/`text_outline_color`/`text_outline_thickness`. This is useful evidence
in two directions: it confirms the *kind* of parameters the user is asking for
(font size, backdrop alpha/rounding) are exactly what a mature overlay in this space
exposes — good validation for the B5 schema below — but it's also concrete proof none
of that surface is reachable from gamescope's side of the message queue; the
`mangoapp_msg_v1` struct (B1) carries none of these fields and has no mechanism to.

### B1 — what mangoapp actually is

`mangoapp.cpp` is **not** an overlay renderer inside gamescope. It's a one-way SysV
message-queue producer: `init_mangoapp()` creates/attaches a queue
(`ftok("mangoapp", 65)`), and `mangoapp_update()`/`mangoapp_output_update()` fill and
`msgsnd(..., IPC_NOWAIT)` a fixed `mangoapp_msg_v1` struct
(`src/mangoapp.cpp:19-35`) containing frametimes, latency, focused PID, output
resolution/refresh, FSR state, HDR/Steam-focus flags, and an engine-name string. A
**separate external process** (a MangoHud build configured to read that same queue
key) is what actually draws anything — gamescope has **no visibility into or control
over** that process's rendering: no font size, no backdrop, no blend mode, nothing.
Whatever gets drawn is styled entirely by the external MangoHud's own config file, out
of reach of this codebase and of any design guide we'd want to match. It's also not
guaranteed to be running at all — `msgsnd` is `IPC_NOWAIT` specifically so a missing
consumer never stalls gamescope (`process-management.md` already documents this).

### B2 — mangoapp vs. drawing our own

| | Reuse mangoapp (spawn/rely on external MangoHud) | Draw in ImGui ourselves |
|---|---|---|
| Font size / backdrop / blending control | **No** — zero API surface into the external process's rendering; would require shipping/patching a MangoHud fork, out of scope | **Yes** — full control, same renderer as the rest of the settings overlay |
| Matches design guide | No | Yes |
| Visible while settings overlay is closed | Only if the external process is separately launched and kept alive — orthogonal lifetime, not something gamescope's overlay toggle controls | Yes, trivially — it's just a second always-drawn ImGui pass gated on its own visibility flag, independent of the settings-panel visibility flag |
| Engineering cost | Low (data plumbing already exists) but capped — can't reach the actual goal | Medium — need an ImGui draw call each frame plus the data plumbing below, but it's new code we fully own |
| Battle-tested | Yes (MangoHud is mature) | No — new code |
| Data needed | Already flows into the message queue | Same underlying numbers, just read directly in-process instead of round-tripping through a message queue and a second process |

**Recommendation: draw it ourselves.** The three named requirements (font size,
backdrop, blending) are rendering controls mangoapp structurally cannot give us since
it doesn't render anything itself — reusing it would mean abandoning those
requirements, not implementing them. The frame-timing math mangoapp already computes
(see B3) should still be reused/mirrored so we're not reinventing "what counts as a
frame," but the draw call should be new ImGui code in-process.

**Lifetime note (must not be missed):** the FPS readout and the settings panel are
different UI elements with different visibility flags. The user's own framing
("visible while the settings overlay is closed") confirms this — it's an always-on
(or independently toggled) HUD element, not a child of the settings panel's open/close
state. This has an implementation consequence for whichever thread/pass draws ImGui:
the FPS text needs to be issued every composited frame regardless of whether the
settings panel is open, so it can't be gated behind the same "is the settings menu
open" branch that will presumably gate the rest of the new ImGui UI.

### B3 — where frame-timing data actually comes from

Three genuinely different numbers exist in this codebase today, already computed at
three different points — and gamescope's own mangoapp bridge already ships all three
in one struct, which is strong evidence for which numbers matter in practice:

1. **The game's own frame rate** — `frametime_ns = lastCommit->present_time -
   w->last_commit_present_time` (`src/steamcompmgr.cpp:7466`, inside
   `handle_presented_for_window`, steamcompmgr thread, gated by
   `!cv_mangoapp_use_output_timing`) — the delta between the focused window's own
   successive buffer commits. This is "how fast is the game actually rendering,"
   independent of whether gamescope shows every one of those frames. Feeds both
   `wlserver_app_presented()` → the `gamescope_control` `app_performance_stats` event
   (B4) and mangoapp's `app_frametime_ns` field.
2. **The compositor's visible/composite rate** — `mangoapp_output_update(vblanktime)`
   (`src/rendervulkan.cpp:2942`, called from the present-wait thread at `:2962` right
   after `GetVBlankTimer().MarkVBlank()`) tracks changes to
   `g_uCurrentBasePlaneCommitID` against vblank time to derive "how often did the
   on-screen image actually change" — this can differ from #1 when a frame is
   reused/dropped. This is mangoapp's `visible_frametime_ns`.
3. **The display's refresh rate** — a fixed configured value, `g_nOutputRefresh` (mHz,
   `src/main.hpp`), exposed to mangoapp as `displayRefresh`
   (`src/mangoapp.cpp:63`,
   `(uint16_t)gamescope::ConvertmHzToHz(g_nOutputRefresh)`). `CVBlankTimer`
   (`src/vblankmanager.hpp:27`, `GetRefresh()`) is the pacing clock for the whole
   vblank-scheduled loop but reports the display's fixed cadence, not a measured
   per-frame value.

**A fourth, distinct mechanism exists but answers a different question.** Gamescope
already implements Wayland's `presentation_feedbacks` path — `commit_t` carries
`presentation_feedbacks` (`src/steamcompmgr.cpp:1401,1415`), consumed at
`src/steamcompmgr.cpp:7479-7485` (`wlserver_presentation_feedback_present`/`_discard`,
`src/commit.cpp:26-29`). Per the [`wp_presentation` protocol's own spec](https://wayland.app/protocols/presentation-time)
(checked 2026-08-21), the `presented` event's timestamp is "when the content update
turned into light the first time on the surface's main output" — a *measured*, not
predicted, scanout time, explicitly distinct from both the client's submission cadence
and the display's nominal refresh interval. Gamescope already sends this feedback
**back to the client game**, i.e. it's the mechanism games use to pace themselves
against gamescope's actual presentation, not a number gamescope surfaces to the user.
It confirms the three-numbers framing above is the right one (submission time,
presentation/scanout time, and refresh cadence are conceptually and mechanically
different Wayland-level things, not just a documentation simplification) but it's not
itself a new data source for the FPS HUD — #1/#2 above (already computed in-process)
remain the right inputs.

**Which one does the user mean?** Almost certainly #1 (the game's FPS) as the
headline number — that's what "FPS counter" conventionally means and what mangoapp
leads with (`app_frametime_ns`). But #2 is the more honest "what you're actually
seeing" number in a compositor, since gamescope can re-show a stale frame; a design
that only shows #1 can read as fine while the compositor is silently missing frames.
Recommend surfacing #1 as the primary number and treating #2 as a secondary/debug
figure — flagging this as a decision for the user rather than assuming (see Open
Questions).

### B4 — `request_app_performance_stats`

`protocol/gamescope-control.xml:135-143`: a client sends `request_app_performance_stats(app_id)`;
server queues the requesting resource (`gamescope_control_request_app_performance_stats`,
`src/wlserver.cpp:1304-1307`, under `wlserver_is_lock_held()`), and fires exactly
**one** `app_performance_stats(app_id, frametime_ns_lo, frametime_ns_hi)` event the
next time `wlserver_app_presented()` runs for that app_id (`src/wlserver.cpp:1310-1328`,
called from `src/steamcompmgr.cpp:7473`). This is the **same #1 game-frametime number**
as B3, just delivered over the external Wayland protocol instead of in-process. It is
explicitly **one-shot, not a stream** — a client re-requests it every sample it wants.
Since the ImGui overlay will live in-process (per the B2 recommendation), it should
read the underlying value directly (mirror the computation at
`src/steamcompmgr.cpp:7466`, or have that site also stash the value somewhere the
ImGui pass can read) rather than round-tripping through this protocol — the protocol
exists for genuinely external clients (Steam) that don't have in-process access.

### B5 — "backdrop" and "blending options" as concrete ImGui parameters

Proposed minimal schema (ranges/defaults are proposals for the user to confirm, not
verified against any existing config since no ImGui code exists yet):

| Field | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `font_size` | float (px) | 10–48 | 18 | ImGui font scale for the FPS text |
| `backdrop_enabled` | bool | – | true | draw a filled rect behind the text |
| `backdrop_opacity` | float | 0.0–1.0 | 0.5 | alpha of the backdrop fill |
| `backdrop_rounding` | float (px) | 0–16 | 4 | `ImDrawList` rect corner radius |
| `backdrop_padding` | float (px) | 0–24 | 6 | inset between text and rect edge |
| `blend_mode` | enum | `alpha` \| `additive` | `alpha` | standard alpha-blend vs. additive (bright/glow look, no dark backdrop needed since additive over dark video usually looks best without a fill) |
| `text_opacity` | float | 0.0–1.0 | 1.0 | independent of backdrop, in case additive mode wants a dimmer readout |

`alpha` is the conventional choice (readable over any content) and should be the
default; `additive` is the "glowing" style some overlays use and pairs oddly with a
backdrop rect (the two options interact — an additive-blended backdrop rect would
itself glow, which may not be intended). Flagging that interaction rather than
resolving it — it's a design-guide call, not a technical one.

---

## Risks

- **ConVar cross-thread writes are not actually synchronized** (`src/convar.h`
  `m_Value` is a plain field, no atomic, no lock) — this is a pre-existing pattern
  (`gamescope_private.execute` on the main thread vs. per-frame reads on the
  steamcompmgr thread) that the new overlay would inherit if it writes ConVars from a
  UI thread that isn't steamcompmgr. Whether this has caused real bugs historically is
  unknown from static reading alone; treat any new cross-thread ConVar/global write as
  guilty until proven safe, and prefer the X11-property or same-thread paths noted in
  A3.
- **Sharpness slider semantics differ per filter** (A4) — a single unified slider
  will visually invert its own meaning when the user switches FSR↔NIS unless the UI
  either re-labels or remaps on the fly. Easy to get subtly wrong; upstream Valve
  looked at normalizing this exact thing and closed it as not planned (external
  corroboration, A4), so this is a UI-layer problem to solve here, not something a
  future upstream merge will fix underneath us.
- **mangoapp's wire struct is a stability contract** (`// WARNING: Always ADD fields,
  never remove or repurpose fields`, `src/mangoapp.cpp`) — if the new in-process FPS
  counter is later asked to also *emit* mangoapp-compatible data (not currently
  planned, but adjacent), that struct's field order/size is load-bearing for external
  MangoHud binaries.
- **`cv_tearing_enabled` liveness not fully traced** (A5) — listed as "likely live"
  by pattern-match to `cv_adaptive_sync`/`cv_hdr_enabled`, not confirmed by reading its
  consumer. Don't promise it in UI copy without a follow-up check.
- **Steam-focus override silently fights the filter UI** — while the Steam window is
  focused, `g_upscaleScaler`/`g_upscaleFilter` are forced to `FIT`/`LINEAR` regardless
  of `g_wanted*` (`src/steamcompmgr.cpp:9320-9325`, flagged in-source as `// XXX(misyl):
  This is bad!`). A settings UI showing "Filter: FSR" while Steam is focused would be
  lying about what's actually being drawn; worth either graying out the control or
  showing the override explicitly in that state.

## Open questions for the user

1. Which frame-rate number should the headline FPS display show — the game's own
   commit rate, the compositor's visible/composite rate, or both (primary + small
   secondary figure)? (B3)
2. Should the sharpness slider be one control that relabels/remaps per filter, or two
   separate sliders (FSR sharpness, NIS sharpness) shown only when that filter is
   active? (A4)
3. Does "additive" blending need to support a backdrop rect at all, or should backdrop
   auto-disable when additive is selected? (B5)
4. Should filter/scaler/sharpness writes go through the existing X11-property
   mechanism (reusing Steam's own live-tuning path) or through new ConVars — the
   latter would need the overlay to guarantee same-thread writes, since ConVar writes
   aren't synchronized? (A2/A3)
5. Is the Steam-focus filter override (Risks) something the UI should surface/explain,
   or is fixing/removing that override itself in scope for this project? It's marked
   `XXX` in-source already.
6. Should the FPS display's own settings (font size/backdrop/blend) persist to the
   same config file/mechanism as the filter settings, or a separate one — affects
   whether this is one schema or two?

## External references

All checked 2026-08-21, cited inline above at point of use:

- [ValveSoftware/gamescope issue #515 — "Make sharpness setting user-friendly"](https://github.com/ValveSoftware/gamescope/issues/515) — upstream confirmation of the FSR/NIS sharpness direction inversion; closed as not planned.
- [flightlessmango/MangoHud — `data/MangoHud.conf`](https://github.com/flightlessmango/MangoHud/blob/master/data/MangoHud.conf) — the config keys MangoHud itself exposes for font size / background alpha / rounding / text outline, used as validation for the B5 parameter schema.
- [`wp_presentation` protocol spec](https://wayland.app/protocols/presentation-time) — primary-source definition of the `presented` event's measured-scanout-time semantics, used in B3 to distinguish submission/presentation/refresh as genuinely different Wayland-level concepts.
- [GamingOnLinux — "AMD FidelityFX Super Resolution comes to Gamescope for the Steam Deck"](https://www.gamingonlinux.com/2022/02/amd-fidelityfx-super-resolution-comes-to-gamescope-for-the-steam-deck/) — secondary-source mention of `Super+U`/`Super+I`/`Super+O` in-game hotkeys for FSR toggle/sharpness, corroborating that filter/sharpness are treated as live state in production; hotkey binding itself not found in this fork's source tree, not further traced.
