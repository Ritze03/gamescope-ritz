# Draft upstream bug report: SDL backend flickers under fullscreen VRR

**Status: DRAFT ONLY — NOT FILED.** This is written to be pasted as-is into a new issue
at `github.com/ValveSoftware/gamescope/issues/new/choose`, once the user (who owns the
GitHub account this would be filed under) decides to file it. No agent has posted this,
or anything derived from it, to any GitHub repository, and none should.

This is the conclusion of a long investigation across this repo's `superdoc/planning/`
docs (`flicker-ab-test-plan.md`, `upstream-flicker-regression.md`,
`client-variation-test.md`, `wayland-vrr-buffer-lifetime.md`,
`overlay-presentation-architecture.md`) — all of which chased other theories (this
fork's own diff, an 83-commit upstream regression window, an optimisation/VRR-engagement
confound, a buffer-refcount race) before the actual, much simpler cause was found. Each
of those docs now carries a resolution note pointing here.

---

## Existing-issue check (done before drafting this)

Searched `ValveSoftware/gamescope` issues (GitHub search API, read-only) for
combinations of: `SDL backend`, `flicker`, `VRR`, `adaptive-sync`, `corruption`,
`tearing`, `Hyprland`, `nested`, `triangles`, `black blocks`, `paused`. **No existing
issue matches this report** (same backend, same symptom, same trigger conditions). Two
issues are related but distinct, worth cross-linking in the filed report rather than
treating as duplicates:

- **[#2140](https://github.com/ValveSoftware/gamescope/issues/2140) — "SDL nested
  surface has obvious corruption at the bottom of a 4K screen."** Also SDL-backend-only,
  also visual corruption, but a different symptom (corruption confined to the bottom of
  a 4K output, reproduced by scrolling a settings menu) and no VRR/adaptive-sync
  involvement mentioned. Possibly a related SDL-presentation-path issue, not confirmed
  the same bug.
- **[#1957](https://github.com/ValveSoftware/gamescope/issues/1957) (closed) — "Nested
  Wayland fails VRR in Hyprland."** Same host compositor (Hyprland) and VRR context, but
  reports the **opposite backend attribution**: that user states "Issue doesn't appear
  with the SDL backend in Hyprland... The issue with `--backend wayland` is severe on
  Hyprland" — and a different symptom (refresh rate oscillating between the MangoHud FPS
  limit and the panel max, not visual corruption). Worth noting as a counterpoint in the
  filed report, not evidence against this one — different symptom, different mechanism,
  and that report never isolated backend/optimisation-level confounds the way this
  investigation did.

If you are about to file this, re-run this search first — time has passed since this
draft was written.

---

## Summary

The SDL backend (`--backend sdl`) flickers/corrupts under fullscreen VRR; the Wayland
backend (`--backend wayland`, or no `--backend` flag at all under a Wayland session,
since `auto_select_backend()` picks Wayland by default) does not, with every other
condition held identical. Reproduced on upstream `3.16.24` (both a from-source debug
build and the distro-packaged release build) through upstream HEAD as of this fork's
base commit `fcc1341` — i.e. it is present across at least 83 commits of upstream
history and both build types, so it is not a recent regression and not an
optimisation-level artifact.

## Environment

- **GPU:** AMD Radeon RX 7900 XTX (RADV NAVI31), Mesa/RADV driver stack.
- **Host compositor:** Hyprland.
- **Display:** `DP-1`, 1920×1080 @ 280 Hz, with VRR enabled on the output
  (Hyprland `vrr = 3`, i.e. "VRR on for a fullscreen window whose content type is
  game/video" — engages automatically for any fullscreen gamescope window regardless of
  gamescope's own `--adaptive-sync` flag).
- **Session type:** Wayland (`WAYLAND_DISPLAY` set).
- **gamescope versions tested:** `3.16.24` (tag, both a debug/`-O0` from-source build and
  the distro-packaged optimised release build) and upstream HEAD at commit `fcc1341`.
  Same result on all of them.

## Exact reproduction command

```
DISABLE_LSFG=1 ./build/src/gamescope --backend sdl -f -- vkcube
```

(`DISABLE_LSFG=1` is only needed if a system-wide implicit Vulkan layer like `lsfg-vk`
is installed — irrelevant to gamescope itself, included here only so the command is
copy-pasteable as run.) The fuller form used through most of this investigation, for
reference:

```
DISABLE_LSFG=1 ./build/src/gamescope --backend sdl -W 1920 -H 1080 -w 1920 -h 1080 \
  -r 280 -O DP-1 -f --adaptive-sync -- vkcube
```

**The clean control, same binary:**

```
DISABLE_LSFG=1 ./build/src/gamescope -W 1920 -H 1080 -w 1920 -h 1080 \
  -r 280 -O DP-1 -f --adaptive-sync -- vkcube
```

(No `--backend` flag — `auto_select_backend()` in `src/main.cpp` picks Wayland when
`WAYLAND_DISPLAY` is set, which is every real Wayland-session user. This is why the bug
went unnoticed: a real user's own session never takes the SDL path unless they pass
`--backend sdl` explicitly.)

## Observed symptom

Black blocks and triangular regions of corrupted/stale content appear across the
fullscreen output. Severity is content-dependent:

- **Worse when the client is paused/idle** (e.g. `vkcube`'s spacebar pause, or
  `SIGSTOP`-freezing the client process) than during active rendering.
- **Cleared by anything that forces a fresh repaint** — e.g. taking a screenshot via
  gamescope's own screenshot path, or any event that makes gamescope commit a new frame.
- Present with `vkcube` (native Vulkan WSI path) under fullscreen + VRR; not
  characterized here against other clients or non-fullscreen/non-VRR configurations
  beyond what's noted below.

## What is and is not required to trigger it

**Required:**
- `--backend sdl` (explicit, or via whatever selects SDL on your session type).
- Fullscreen (`-f`).
- A VRR-capable output with VRR actually engaging — either via gamescope's own
  `--adaptive-sync`, or via the host compositor applying VRR to the fullscreen window
  unconditionally (as Hyprland's `vrr = 3` mode does here) even when gamescope's own
  `--adaptive-sync` is omitted.

**Not required:**
- Not specific to any one gamescope version — reproduces on stock `3.16.24` through
  `fcc1341` (83 commits of upstream history), debug and release builds alike.
- Not caused by any downstream/fork-specific code: reproduces byte-for-byte identically
  on pure upstream builds with zero modifications.
- Not related to build optimisation level — confirmed flickering on both a debug/`-O0`
  build and a distro-optimised release build of the same `3.16.24` source.
- Not related to the Steam overlay, an FPS HUD, or any upscaling filter — reproduces
  with plain `vkcube` and no overlay/HUD code running at all.
- **The Wayland backend does not reproduce this at all**, under otherwise identical
  conditions (same hardware, same host compositor, same output, same VRR state, same
  client). This is the single most decisive fact gathered: it rules out the display,
  the driver, the host compositor, and gamescope's shared compositing/present-mode code
  as the cause, and narrows it to something specific to `src/Backends/SDLBackend.cpp`.

## Source-level observation (not a confirmed root cause — included because it is a
concrete, evidence-backed lead, not speculation)

`src/Backends/SDLBackend.cpp`'s `CSDLConnector` unconditionally reports no VRR support
or activity, regardless of the actual `--adaptive-sync` flag or host state:

```cpp
bool CSDLConnector::IsVRRActive() const
{
    return false;
}
...
bool CSDLConnector::SupportsVRR() const
{
    return false;
}
```

Contrast with `src/Backends/WaylandBackend.cpp`'s `CWaylandConnector`, which reports the
*actual* host VRR state:

```cpp
bool CWaylandConnector::SupportsVRR() const
{
    return CurrentDisplaySupportsVRR();  // queries HostCompositorIsCurrentlyVRR()
}
```

`IsVRRActive()`/`SupportsVRR()` are not cosmetic — they gate real frame-pacing and
paint-scheduling decisions, at minimum in:

- `src/vblankmanager.cpp`'s `CVBlankTimer::CalcNextWakeupTime()` — when `IsVRRActive()`
  is `false`, it applies a fixed-refresh "redzone" margin calculation (scaled off the
  target refresh rate) intended for a *non*-variable display, explicitly to avoid
  missing the vblank deadline on a fixed cadence.
- Multiple call sites in `src/steamcompmgr.cpp` (paint scheduling, frame-limiter
  behaviour, and VRR-specific code paths) that branch on `IsVRRActive()`/`SupportsVRR()`.

Under the SDL backend with a host compositor that applies its own VRR to the fullscreen
window regardless of gamescope's flag (as observed here with Hyprland's `vrr = 3`),
gamescope's internal pacing logic believes it is targeting a **fixed**-refresh display
and paces/redraws accordingly, while the actual scanout beneath it is running
**variable**-refresh. That mismatch between gamescope's internal timing model and the
display's real behaviour is a plausible mechanism for stale/torn scanout content
appearing under exactly the conditions reproduced here (worse when idle — gamescope
skips redraws it thinks it doesn't need to hit a fixed vblank target — cleared by a
forced repaint). **This has not been confirmed as the root cause** — it is a source
reading, not an instrumented proof — but it is concrete enough to be a reasonable
starting point for whoever picks this issue up on the SDL backend.

## Why this was hard to find

Every automated test script in the fork that produced this investigation's own test
tooling (`scripts/flicker-ab-test.sh`, `scripts/overlay-test-harness.sh`) defaulted
`--backend` to `sdl`, for an unrelated reason (matching what looked like the user's
"nested under the host compositor" setup). Every "flickers" result the investigation
produced, across many rounds of testing different hypotheses, went through that broken
default. Every "clean" result was either a manual run with no `--backend` flag, or the
user's actual gaming sessions — both of which take the Wayland path. Both script
defaults have since been corrected (no `--backend` flag by default, i.e. gamescope's own
`auto_select_backend()` decides, same as a real user gets) — see the "WHY NOT SDL"
comment near the top of each script.
