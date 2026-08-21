# Wayland backend, VRR host: buffer lifetime findings

**2026-08-21.** An instrumented pass over `src/Backends/WaylandBackend.cpp` to test one
specific hypothesis about the fullscreen VRR flicker, plus two findings that came out of
it. Written so the next agent does not re-run any of this.

## Test rig (reproduce these conditions or the results mean nothing)

Host Hyprland, DP-1 1920x1080@280Hz, `vrr = 3`. gamescope nested, `--backend wayland`,
`-f`, client `vkcube`, `DISABLE_LSFG=1`. Every run verified **read-only** with
`hyprctl clients -j` (gamescope window `fullscreen: 2`) and `hyprctl monitors -j`
(DP-1 `vrr: true`) *while the run was live* — not before or after, which reports
`vrr: false` because nothing is fullscreen then.

**Always record the achieved frame rate and the build type.** Both have already
invalidated conclusions in this investigation, and both look like "it's intermittent".
VRR only varies the refresh interval *below* the panel maximum; above it the panel pins
at max, which is fixed refresh, so an uncapped run does not test VRR at all.

Frame rate was measured two ways, both binary-agnostic:
- Instrumented builds: count host commits per second directly from the acquire log.
- Any build: `vkcube --c N` exits after N frames, so timing two runs with different N
  and taking the slope `(N2-N1)/(t2-t1)` cancels startup cost.

## Finding 1 — the optimisation confound (the important one)

See the banner at the top of `upstream-flicker-regression.md`. 3.16.24 built debug/`-O0`
runs at **231 fps**; the same source as a packaged release build runs at **282 fps**.
The 280Hz panel maximum sits between them, so optimisation level alone decides whether
VRR engages. The "stock gamescope is clean" comparison that started the 83-commit
bisection very likely measured optimisation level, not source version.

Wired up as `scripts/flicker-ab-test.sh upstream-3.16.24-debug` and
`packaged-3.16.24-release` so this is cheap for the user to confirm.

## Finding 2 — the acquire/release hypothesis is DISPROVED

The hypothesis: `CWaylandFb::m_bCompositorAcquired` was a bool, not a count, so a
re-presented buffer acquires once but releases twice; the early release was supposed to
free a buffer while the host still scanned it out, and the "Compositor released us but we
were not acquired" warning was the detector nobody had run under the right conditions.

Instrumented with a shadow count of outstanding host references, logging every acquire
and release with the refcount, the backing `wlr_buffer`, and the explicit-sync release
point. Three independent results kill it:

1. **In the exact failing regime it never fires.** Fullscreen, DP-1 `vrr: true`,
   `--framerate-limit 140` giving a *measured* 152.6 presents/s (well below 280, so VRR
   genuinely varies), debug `-O0`: **0 double-acquires, 0 early releases, max outstanding
   references = 1**, over 3025 presents in 20s.
2. **The warning is anti-correlated with the symptom.** Stock 3.16.24 fires it
   212-523 times per run in the *release* build the user reports as **clean**, and only
   27-69 times in the debug build. A detector that fires ten times more often on the
   clean binary is not detecting this bug.
3. **It could not cause the symptom anyway on the path we can observe.** Every
   `CWaylandFb` presented in these runs had `m_pClientBuffer == nullptr` and no release
   point, because vkcube's layer is not flippable (`Gamescope WSI ... flip: false`) so
   gamescope takes the full-composite path and presents its *own* output image. Dropping
   that refcount to zero hands nothing back to any client.

*Why the earlier pass saw it 3x at mode transitions and 0x in steady state:* the vast
majority of double-acquires belong to the **black backing FB**, which is re-presented
every frame, is owned outright by the backend, and has no client buffer — so it is noise.

**It was still a real bug and it is now fixed** (see below) — just not this bug. Do not
re-litigate it; re-read this section instead.

## Finding 3 — gamescope goes completely silent when the client stops presenting

Freezing the client with `SIGSTOP` (the keyboard-free equivalent of vkcube's spacebar
pause, which is the user's "breaks every single time" case) produced **zero
`wl_surface` commits for the entire 20-second pause** — the next host commit came
20.0s later, immediately after `SIGCONT`.

*Why this matters:* under VRR the host has nothing new to show either, so the output
stops flipping entirely and the panel free-runs at the bottom of its VRR range for as
long as the pause lasts. That is consistent with every user observation that the
refcount hypothesis fails to explain: VRR off is clean (fixed refresh re-scans the same
image safely), pausing breaks it every time (the longest possible gap), a screenshot
clears it (it forces a repaint), and the corruption is stale scanout content rather than
anything in gamescope's composited image.

**This is the strongest surviving lead and it is NOT yet proven.** It needs the user's
eyes, and it is not yet established whether the right fix is in gamescope (keep
committing at the panel's minimum VRR rate when content is static, the way the DRM
backend's LFC handling does) or in the host/driver. Unresolved: it does not obviously
explain flicker during *normal* play at ~180 fps, where the gaps are ~5.5ms and well
inside the VRR range.

## What was changed

`CWaylandFb::m_bCompositorAcquired` (bool) became `m_uCompositorAcquisitions` (count).

*Why:* the host sends exactly one `wl_buffer.release` per attach+commit, and
`CWaylandConnector::Present()` re-attaches the current buffer to every plane surface each
frame and always follows each plane's `Present()` with a `Commit()`, so acquisitions
genuinely nest. A bool swallowed the second `IncRef()`, and then the *first* release
dropped gamescope's reference while the host still held the buffer for the second attach.
On the direct-scanout path (`SetBuffer`/`SetReleasePoint` set from `steamcompmgr.cpp`)
that would `wlr_buffer_unlock` the game's buffer, or signal its explicit-sync release
point, while the host was still scanning it out.

*Why a strict count cannot leak:* the one failure mode would be a host that coalesces
releases, which would make references climb forever. Measured against Hyprland it does
not — with the bool version the shadow count stayed bounded at ≤3 across 11,400
double-acquires, and after the fix a 50s run held file descriptors constant at 103 with
flat RSS and **0** "not acquired" warnings (down from 212-523 on stock).

Upstream-reportable as a standalone correctness fix. There is no matching upstream issue.
It should be reported as what it is — a latent buffer-lifetime bug on the direct-scanout
path — and explicitly **not** as a fix for the VRR flicker, which it does not fix.
