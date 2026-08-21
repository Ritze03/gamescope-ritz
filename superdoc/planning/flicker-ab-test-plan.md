# Flicker A/B test plan

> **RESOLVED — 2026-08-21.** The flicker this plan was chasing is an **SDL-backend
> bug**, confirmed on real hardware: `DISABLE_LSFG=1 ./build/src/gamescope --backend
> sdl -f -- vkcube` flickers badly; the identical binary with **no `--backend` flag**
> (which auto-selects Wayland — see `auto_select_backend()` in `src/main.cpp`) is
> completely clean. The user's packaged upstream 3.16.24 also flickers under SDL, so
> this is an **upstream SDL-backend defect, not ours**, and version-independent. It
> went unnoticed because a real user on a Wayland session never takes the SDL path —
> but `scripts/flicker-ab-test.sh` defaulted `--backend` to `sdl`, so **every single
> "flickers" result this plan ever produced went through the broken backend**, and
> every "clean" result (the `upstream` control here included, when run manually
> without `--backend`) was actually a Wayland-backend run. The premise below —
> "the cause is somewhere in our diff against upstream" — is **wrong**: there is no
> such diff-based cause. The script's `--backend` default has been changed to auto
> (no flag, i.e. gamescope's own auto-selection) — see the "WHY NOT SDL" comment near
> the top of `scripts/flicker-ab-test.sh`. The reasoning below is kept for the record
> but every conclusion in it is superseded by the above. Draft upstream report:
> `superdoc/planning/upstream-sdl-backend-flicker-report.md`.

**2026-08-21.** Written for a ten-minute testing session, not an hour. You are the
instrument here — nothing available in this repo can measure the artifact, only your
eyes on real hardware can. Ground truth so far: stock `/usr/bin/gamescope
--adaptive-sync`, fullscreen on DP-1 at 280 Hz, is clean; our build in the same
conditions flickers, independent of the settings overlay, the FPS HUD, and the filter.
So the cause is somewhere in our diff against upstream (base commit `fcc1341`, which is
exactly upstream `ValveSoftware/gamescope` HEAD).

All variants below (except the `upstream` control) are **one binary** —
`build/src/gamescope` — flipped by environment variable, so you never rebuild between
tests. Run each with `scripts/flicker-ab-test.sh`; it handles fullscreen, VRR,
`DISABLE_LSFG=1`, a throwaway config directory, and clean teardown for you.

## One-time setup (do this once, ~3 minutes)

1. Build our HEAD, if you haven't already:
   ```
   meson setup build/
   ninja -C build
   ```
2. Build the upstream control (fcc1341, this fork's exact upstream base) in its own
   directory, once:
   ```
   git worktree add ../gamescope-upstream-fcc1341 fcc1341 --detach
   cd ../gamescope-upstream-fcc1341
   git submodule update --init --recursive
   meson setup build/ -Denable_tests=false
   ninja -C build
   cd -
   ```
   (`-Denable_tests=false`: at fcc1341 the test suite depends on a Catch2 wrap that
   doesn't exist yet at that commit — irrelevant here, we only need the `gamescope`
   binary itself.) If you built it somewhere other than `../gamescope-upstream-fcc1341`
   relative to this repo, point the script at it with
   `GAMESCOPE_RITZ_AB_UPSTREAM_BIN=/path/to/it/build/src/gamescope`.
3. Make sure DP-1 is your **focused** monitor before each run (click on it, or move
   your mouse there) — the script uses gamescope's own `-f` flag to fullscreen on
   whichever monitor the window opens on, and deliberately does **not** drive Hyprland
   to move it there for you (a previous automated attempt at that closed one of your
   windows). It prints a clear warning if DP-1 wasn't focused, and after launch it
   tells you plainly whether fullscreen actually happened.

Every run below uses `vkcube` as the test client by default (`--client CMD` to change
it) and never touches `~/.config/hypr` or your real gamescope config.

## The runs, in order

Watch DP-1 for a few seconds after each launch, note flicker/clean, then `Ctrl+C` in
the terminal to tear down cleanly before the next run.

### 1. Overlay fully inert — the most informative single test

```
scripts/flicker-ab-test.sh overlay-inert
```

This is our HEAD, but the settings overlay and FPS HUD are skipped **entirely** at the
call site that would normally add them — no ImGui context is ever created, no
offscreen texture is ever allocated, no compositor layer is ever pushed, for the whole
process lifetime. This is a stronger claim than "the overlay is closed": it removes the
overlay subsystem from the process's behaviour altogether, closing the gap between
"probably not the cause" (your independence observation) and "definitely not the
cause".

- **Still flickers** → the overlay/FPS HUD subsystem is exonerated. The cause is
  elsewhere in our diff. Go to run 3.
- **Clean** → contrary to the independence observation, the overlay subsystem *is*
  somehow involved (e.g. something it changes at device-creation time persists even
  when the overlay itself never draws — see run 4, which isolates exactly that).
  Re-run #2 immediately after to make sure the flicker you'd been seeing wasn't
  session drift.

### 2. Normal — reconfirm the known-bad case, back to back with #1

```
scripts/flicker-ab-test.sh normal
```

Our HEAD, completely unmodified. Should flicker, matching what you've already seen.
Running it right after #1 on the same session/hardware state is the sanity anchor: if
this ever comes out clean, something about *this session* changed, not the build —
distrust the whole run and start over.

### 3. Without VRR at all

```
scripts/flicker-ab-test.sh normal --no-vrr
```

You said you're willing to test whether it flickers even without VRR — this is that
test, done the clean way (gamescope's own `--adaptive-sync` flag omitted entirely,
nothing touched in Hyprland). Toggling VRR off through the overlay didn't help before;
this rules out the overlay's toggle path itself being the problem, separately from
whether VRR is involved at all.

- **Still flickers with VRR fully off** → this is not a VRR-specific artifact; whatever
  it is happens on the plain scanout path too. Narrows things a lot.
- **Clean without VRR** → it is specifically a VRR interaction — worth revisiting the
  adaptive-sync-related code paths specifically, even though runs 1/4/5 didn't find it.

### 4. Device-creation: the dynamic-rendering extension string

```
scripts/flicker-ab-test.sh no-dynamic-rendering-ext
```

Our HEAD, but device creation does **not** request the `VK_KHR_dynamic_rendering`
extension string (added in M1 for the overlay; the `VkPhysicalDeviceVulkan13Features`
feature bit itself, already core in Vulkan 1.3, is left untouched — only the extension
string is withheld). This is one of the few changes in our diff that affects device
creation for the *whole* compositor, not just the overlay, so it's tested independently
of whether the overlay ever draws.

- **Clean** → this extension string is implicated. Worth understanding why requesting a
  promoted-to-core 1.3 extension changes scanout behaviour on this driver.
- **Still flickers** → exonerated, move on.

### 5. Startup ConVar seeding

```
scripts/flicker-ab-test.sh no-convar-seed
```

Our HEAD, but the startup step that seeds `cv_adaptive_sync`/`cv_hdr_enabled`/
`cv_tearing_enabled` (and the filter/scaler/sharpness globals) from the gamescope-ritz
config system — which runs before argv parsing — is skipped. An explicit
`--adaptive-sync` on the command line still applies as normal either way, so this only
matters if your config file sets any of these away from their (matching-upstream)
`false` defaults. Low-probability suspect if you're using a fresh/throwaway config (the
script's `XDG_CONFIG_HOME` always is), but cheap to rule out.

- **Clean** → the seeding step, or something it touches, is implicated.
- **Still flickers** → exonerated.

### 6. Upstream control — does *this test rig* itself agree with your original observation

```
scripts/flicker-ab-test.sh upstream
```

The tree built at `fcc1341` exactly, in its own build directory (see setup step 2). If
this flickers *on this same rig, launched this same way*, something about how this
harness builds or launches gamescope differs from your packaged `/usr/bin/gamescope`
--- and that difference, not anything in our diff, would be the real finding. If it's
clean (expected, matching your original observation), it confirms every comparison
above is trustworthy.

## Reading the overall result

| overlay-inert | no-dyn-rendering-ext | no-convar-seed | upstream | Conclusion |
|---|---|---|---|---|
| clean | — | — | clean | Overlay subsystem is the cause (device-creation side-effects most likely — see run 4's note on the extension string, plus `bGeneralQueueShared`/CONCURRENT sharing on overlay textures, both only reachable when the overlay code runs at all) |
| flickers | clean | — | clean | The dynamic-rendering extension string is the cause |
| flickers | flickers | clean | clean | The startup ConVar seeding is the cause |
| flickers | flickers | flickers | clean | None of the three prime suspects are it — the cause is somewhere else in the fcc1341..HEAD diff. Next step: bisect commit-by-commit between `fcc1341` and `HEAD` with the `normal` variant's launch command, since the overlay and the two device-creation suspects are now all cleared |
| flickers | flickers | flickers | flickers | The comparison itself is unsound on this rig — stop drawing conclusions from these variants and go figure out what differs between this build/launch path and your packaged `/usr/bin/gamescope --adaptive-sync` (build flags, driver env vars, etc.) |

Cross-reference run 3 (`--no-vrr`) against whichever row you land on: if it's also
clean without VRR, the cause is VRR-specific regardless of which subsystem it's in; if
it still flickers without VRR, it isn't.

## Script reference

`scripts/flicker-ab-test.sh <variant> [options]` — `--help` for the full option list.
Variants: `overlay-inert`, `no-dynamic-rendering-ext`, `no-convar-seed`, `normal`,
`upstream`. Useful options: `--no-vrr`, `--output NAME` (default `DP-1`), `--client
CMD` (default `vkcube`), `--backend NAME` (default: none passed — gamescope
auto-selects, same as a real user's session; **no longer `sdl`**, see the resolution
note at the top of this doc — SDL is the confirmed bug).
