# OpenVR Backend — output as a SteamVR overlay

Renders gamescope's composited output into a SteamVR overlay instead of a physical display,
so a nested desktop or app can live inside a VR headset. It implements the same
`IBackend` (`src/backend.h:312`) contract as the other backends — see
[backend-drm](backend-drm.md), [backend-sdl](backend-sdl.md),
[backend-wayland](backend-wayland.md), [backend-headless](backend-headless.md) — but each
`COpenVRConnector` (`src/Backends/OpenVRBackend.cpp:311`) maps to one SteamVR overlay
plane rather than one physical connector, and visibility is driven by SteamVR events
instead of hotplug.

## How it works

- `COpenVRBackend` (`src/Backends/OpenVRBackend.cpp:441`) owns the set of active
  connectors under `m_mutActiveConnectors` (`:1487`) and two atomics that name the
  connector currently entitled to keyboard and mouse input:
  `m_pKeyboardFocusConnector` / `m_pMouseFocusConnector` (`:1488`-`:1489`).
- **Normal path — SteamVR grants focus.** `COpenVRBackend::SetMouseFocus` /
  `SetKeyboardFocus` (`:1049`, `:1089`) fire from SteamVR input-focus-changed events, look
  up which connector's plane owns the newly-focused overlay handle
  (`GetPlaneByOverlayHandle`, `:1037`), swap the focus-connector atomic, and call
  `MakeFocusDirty()` + `nudge_steamcompmgr()` so the steamcompmgr thread rerolls focus
  on its next pass (see [steamcompmgr-focus](steamcompmgr-focus.md) for what happens on
  that side).
- **Visibility path — a connector becomes visible.** `COpenVRConnector::UpdateVisibility`
  (`src/Backends/OpenVRBackend.cpp:1842`) is the single place `m_bOverlayShown` /
  `m_bSceneAppVisible` transitions are observed (fed by `MarkOverlayShown` /
  `MarkSceneAppShown`, `:405`/`:412`). Since commit `fcc1341`, on the visible-edge it
  also **takes** keyboard and mouse focus for itself, rather than only reacting to a
  grant:
  ```cpp
  bool bTakeKeyboard = !pKeyboardConnector || pKeyboardConnector == this ||
      !pKeyboardConnector->GetPlaneByOverlayHandle( g_FocusedVROverlayKeyboard.load() );
  ```
  *Why:* SteamVR can park input focus on nothing while a launching app's overlay
  flickers through hide/show, and nothing regrants it until the user clicks. Left
  waiting on the grant, gamescope keeps publishing the *previous* app's focused-app
  atoms, skips the wlserver focus handoff, and MangoHud (mangoapp) never gets nudged —
  it sits frozen until the first click. Taking focus on becoming visible closes that
  window. The `pKeyboardConnector->GetPlaneByOverlayHandle(...)` check is the guard
  against self-theft: it only takes focus when the current grant does **not** already
  point at *this* connector's own overlay, so a connector becoming visible can never
  steal focus SteamVR deliberately handed to a different, currently-visible connector.
  This does not fix per-app controller activation (that still follows SteamVR's own
  grant) — it only keeps gamescope's own focus bookkeeping correct while that first
  launch is dead. This whole take-on-visible behaviour is itself upstream
  `ValveSoftware/gamescope` code, not this fork's — it landed in commit `fcc1341`
  ("OpenVRBackend: take input focus when a connector becomes visible"), which *is*
  this fork's base commit (confirmed exactly equal to upstream HEAD via
  `git merge-base --is-ancestor`). This repo is upstream plus additive overlay work
  on top; it has not diverged from upstream in the OpenVR backend.
- Both paths converge on the same two atomics, so [steamcompmgr-focus](steamcompmgr-focus.md)'s
  `GetCurrentFocus()` / `GetCurrentMouseFocus()` never need to know which path granted
  focus, only which connector currently holds it.
- Every connector carries a `VirtualConnectorKey_t` (`src/backend.h:49`, `uint64_t`),
  the same key steamcompmgr uses to index `g_VirtualConnectorFocuses` — see
  [steamcompmgr-focus](steamcompmgr-focus.md) for the per-connector focus-state model
  this feeds.
- `COpenVRBackend::GetCurrentConnector` / `GetCurrentMouseConnector`
  (`src/Backends/OpenVRBackend.cpp:769`, `:773`) simply return
  `m_pKeyboardFocusConnector` and `m_pMouseFocusConnector` respectively — the same two
  atomics the take-on-visible and SteamVR-grant paths above write — so keyboard and
  mouse focus can legitimately point at different connectors at once (e.g. a VR overlay
  keyboard vs. a scene-app pointer), and [steamcompmgr-focus](steamcompmgr-focus.md)'s
  `GetCurrentFocus()` / `GetCurrentMouseFocus()` resolve to different connector keys
  accordingly. `m_oulCurrentSceneVirtualConnectorKey` (`:1483`) is a separate piece of
  state — which connector's scene app SteamVR currently reports as active — tracked by
  the SteamVR scene-app-changed event handler (`:1182`) and consulted when a connector
  is constructed (`:1829`); it is not read by `UpdateVisibility`'s take-on-visible guard
  or by these two getters.

## Using it

- Select the backend with `--backend openvr` (parsed in `src/main.cpp:441`,
  constructed at `src/main.cpp:990`); only built when `HAVE_OPENVR` is enabled.
- SteamVR overlay identity and behaviour are set via CLI flags at startup — see
  Options below. There is no in-session UI for these; they're one-shot construction
  parameters for the overlay.
- `--vr-session-manager` marks gamescope as the SteamVR session manager overlay rather
  than a plain app overlay.
- Debug: `focus_info` (see [steamcompmgr-focus](steamcompmgr-focus.md)) dumps, per
  connector key, which one currently holds keyboard/mouse focus — the fastest way to
  confirm whether the take-on-visible path or the SteamVR-grant path last won.

## Options

| CLI flag | Meaning |
| --- | --- |
| `--vr-overlay-key` | SteamVR overlay key string for this overlay |
| `--vr-app-overlay-key` | Overlay key to use for child-app overlays |
| `--vr-overlay-explicit-name` | Force the SteamVR overlay display name |
| `--vr-overlay-default-name` | Fallback overlay name when there's no window title |
| `--vr-overlay-icon` | SteamVR overlay icon file |
| `--vr-overlay-show-immediately` | Make the overlay take focus immediately |
| `--vr-overlay-enable-control-bar` | Enable the SteamVR control bar |
| `--vr-overlay-enable-control-bar-keyboard` | Add the keyboard button to the control bar |
| `--vr-overlay-enable-control-bar-close` | Add the close button to the control bar |
| `--vr-overlay-enable-click-stabilization` | Enable SteamVR click stabilization |
| `--vr-overlay-modal` | Present the overlay as a modal |
| `--vr-overlay-physical-width` | Physical width of the overlay, in metres |
| `--vr-overlay-physical-curvature` | Overlay curvature |
| `--vr-overlay-physical-pre-curve-pitch` | Pre-curve pitch |
| `--vr-scroll-speed` | Scroll speed for the VR pointer |
| `--vr-session-manager` | Register as the SteamVR session manager overlay |

Full text lives at `src/main.cpp:239`-`:252` (help output) and the `getopt_long` table
at `src/main.cpp:99`-`:114`.

## Related links

- [steamcompmgr-focus](steamcompmgr-focus.md) — what happens on the steamcompmgr side
  once `MakeFocusDirty()` fires: per-connector focus state, the X11 keyboard-focus
  edge cases, and `focus_info`.
- [Architecture overview](../architecture/overview.md) — backend abstraction and
  threading model.
- [Terminology](../meta/TERMINOLOGY.md) — connector, `VirtualConnectorKey_t`.
