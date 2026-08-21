# Flicker: client/present-mode/backend/scaling variation test

> ## RESOLVED — 2026-08-21 (later same day). The backend WAS the answer, not scaling.
>
> This doc correctly spotted backend as a real variable (finding #1 below) but then
> deprioritized it in favor of the **resolution-scaling hypothesis** ("the standout
> difference", tests #1–#3) as the leading suspect. That hypothesis is **wrong** — it
> is a red herring. The confirmed cause is the **SDL backend itself**, independent of
> scaling: `DISABLE_LSFG=1 ./build/src/gamescope --backend sdl -f -- vkcube` flickers
> badly at 1:1 with no scaling and no extra args at all; the identical binary with no
> `--backend` flag (auto-selecting Wayland) is completely clean, also at 1:1. The
> user's packaged upstream 3.16.24 flickers under SDL too. So test #4 below ("backend
> alone, isolated from scaling") is actually the one that matters — treat it as the
> primary test, not the fallback-if-ambiguous one plan order below makes it. Every
> "flickers" result up to this point traces back to `scripts/flicker-ab-test.sh`
> defaulting `--backend` to `sdl` (now fixed — see the "WHY NOT SDL" comment near the
> top of that script). Draft upstream report:
> `superdoc/planning/upstream-sdl-backend-flicker-report.md`.

**2026-08-21.** Every test run so far in this investigation (see
`superdoc/planning/flicker-ab-test-plan.md` and
`superdoc/planning/upstream-flicker-regression.md`) held four things constant that the
user's real, never-flickering gaming session does **not** hold constant. This doc adds
those variables to `scripts/flicker-ab-test.sh` and orders the tests cheapest/most
decisive first.

## The user's real launch (captured live via `ps aux`, read-only, while CS2 was running)

```
gamescope -W 1920 -H 1080 -w 1280 -h 960 -S stretch --adaptive-sync --immediate-flips \
          --force-grab-cursor --expose-wayland --filter fsr --sharpness 5 -f -- %command%
```

No `--backend` flag. Every test run in this investigation so far passed `--backend sdl`
explicitly, or used `-W`/`-H` with **no** `-w`/`-h` at all (1:1, no scaling).

## What differs, and what's now objectively established (no eyes-on-screen needed)

1. **Backend.** `src/main.cpp`'s `auto_select_backend()`: with no `--backend` flag, if
   `WAYLAND_DISPLAY` is set it picks **Wayland**, only falling to SDL if just `DISPLAY`
   is set, DRM otherwise. This machine's session has `WAYLAND_DISPLAY=wayland-1` set
   (confirmed via `env`) — so the user's real game runs the **Wayland** backend, not the
   `sdl` default every prior test used.
2. **Nested vs. output resolution — the standout difference.** The user's launch is
   `-w 1280 -h 960` against `-W 1920 -H 1080`: the client renders smaller than the
   output and gamescope scales every frame. Every test in this investigation used
   `-W`/`-H` with no `-w`/`-h`, i.e. nested == output, no scaling. This is a real,
   code-visible fork: `commit_t::ShouldPreemptivelyUpscale()` (`src/commit.cpp:149`)
   returns `false` whenever `calc_scale_factor()` comes out `close_enough(1.0, 1.0)` —
   i.e. **only** at 1:1. When it's `true` (the scaled case), `steamcompmgr.cpp` around
   line 7756 runs an *extra*, asynchronous pre-composite pass onto a temp image, gated
   on `fifo`-only commits, with its own semaphore/timeline and a `static
   s_ulLastPreemptiveUpscaleSeqNo` carried frame-to-frame — real additional
   cross-frame synchronization state that the 1:1 path never touches. Whether *this*
   is the mechanism is unconfirmed, but the fork is real and untested until now.
3. **`--immediate-flips` is present in the working config.** Its own `--help` text says
   "may result in tearing" — but since the user sees no artifacts with it *on*, this
   flag is exonerated as an explanation on its own.
4. **Client and present mode** (the original scope of this doc, now secondary — see
   below): every run used `vkcube` with no `--present_mode` flag. Checked against
   upstream `Vulkan-Tools/cube/cube.cpp`: the built-in default is already
   `VK_PRESENT_MODE_FIFO_KHR`, so an explicit `--present_mode 2` should be a
   behavioural no-op vs. no flag. Available on this machine without installing
   anything: `vkcube`, `vkcubepp`, `vkgears`, `glxgears`. **Not** installed:
   `vkcube-wayland`, `glmark2`.

## New script flags (`scripts/flicker-ab-test.sh --help` has the full text)

- `--nested-width W` / `--nested-height H` — set `-w`/`-h` independently of
  `-W`/`-H` (default: same as output, i.e. 1:1 — the untested-until-now default every
  prior run used implicitly).
- `--extra-args "STR"` — raw extra gamescope flags, whitespace-split (e.g. the user's
  `-S stretch --immediate-flips --filter fsr --sharpness 5 --force-grab-cursor
  --expose-wayland`).
- `--client CMD` — test client (default `vkcube`).
- `--present-mode N` — appends `--present_mode N` to a vkcube-family client only
  (0 immediate, 1 mailbox, 2 fifo, 3 fifo-relaxed); warns and no-ops on other clients.
- `--backend NAME` already existed; now doubly important given finding #1 above.

## Test order — scaling/backend first (this is the priority lead), then client/present-mode

Every command below uses `packaged-3.16.24-release` (the user's actual
`/usr/bin/gamescope` binary) as the constant, per the brief — it's the binary they
game with, so it's the one binary a "clean" or "flickers" result actually settles
something about.

### 1. Cheapest decisive test: force scaling onto our exact failing case

```
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland \
  --nested-width 1280 --nested-height 960
```

Everything else stays as the known-flickering baseline (`vkcube`, no extra flags).
**If this alone goes clean, the resolution-scaling hypothesis is confirmed** — that
was the entire delta. This is one flag away from every failing run before it.

### 2. Confirm from the other direction: force 1:1 onto the user's own working flags

```
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland \
  --nested-width 1920 --nested-height 1080 \
  --extra-args "-S stretch --immediate-flips --filter fsr --sharpness 5 --force-grab-cursor --expose-wayland"
```

Take the known-clean flag set and remove only the scaling. **If this alone breaks it**,
that's corroborating evidence from the opposite direction — strong enough on its own,
decisive together with #1.

### 3. Control: the user's exact launch, unmodified, run through our harness

```
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland \
  --nested-width 1280 --nested-height 960 \
  --extra-args "-S stretch --immediate-flips --filter fsr --sharpness 5 --force-grab-cursor --expose-wayland"
```

Should be clean — if it isn't, something about *this harness* (not the hypothesis)
differs from how Steam actually launches gamescope, and #1/#2 need to be re-read with
that in mind before trusting them.

### 4. Backend alone, isolated from scaling (only run if 1–3 are ambiguous)

```
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend sdl
```

Both at 1:1 (default nested size), otherwise identical. Separates "backend matters" from
"scaling matters" if 1–3 didn't cleanly land on one cause.

### 5. Client / present-mode variation (secondary — only if 1–4 don't explain it)

```
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland --present-mode 0
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland --present-mode 2
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland --client vkgears
scripts/flicker-ab-test.sh packaged-3.16.24-release --backend wayland --client glxgears
```

`--present-mode 2` (FIFO) should behave like the no-flag default (source-verified, see
above) — if it doesn't, that itself is a finding about the client, separate from
present mode per se. `vkgears`/`glxgears` exercise the GLX/Xwayland path instead of the
native Vulkan WSI path vkcube uses — a clean result there while vkcube flickers would
point at the Vulkan WSI layer specifically, not compositing in general.

## What to record for every case

Client, present mode, backend, nested vs. output resolution (1:1 or scaled), and
whether corruption appears. The script's banner line now prints `nested (-w/-h)` and
whether it's `(1:1, no scaling)` or `(SCALED to output)` so this is visible at a
glance in the log, no separate note-taking needed.

## One live measurement already taken (packaged 3.16.24, `--backend sdl`, 1:1, `vkcube --present_mode 2`, DP-1 @ 280Hz, `--adaptive-sync`)

Via gamescope's `-T`/`--stats-path` FIFO (`fps=` lines), two short runs: **~272 fps**
and **~282 fps** — i.e. right at or just under the 280Hz cap either way, meaning VRR
barely engages (if at all) even at FIFO in the 1:1/sdl case. Note: `-T` made the
packaged binary abort on `SIGTERM` teardown in both runs (after already having written
useful data) — treat `-T` as informational-only on this build, not something to lean
on for anything beyond a quick fps read, and always give it its own throwaway run.
