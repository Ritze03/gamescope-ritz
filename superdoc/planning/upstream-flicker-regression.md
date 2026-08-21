# Upstream flicker regression: bisection plan

**2026-08-21.** Written after `superdoc/planning/flicker-ab-test-plan.md` fully cleared
our own diff: **pure upstream `fcc1341`, built by us, flickers on DP-1 1920x1080@280Hz**,
and the user's packaged `/usr/bin/gamescope` `3.16.24` does not. So the regression is one
(or more) of the **83 commits** between the `3.16.24` tag and `fcc1341` in
`ValveSoftware/gamescope` — none of it is ours. This doc is the plan for finding which
one, spending as few of the user's eyes-on-screen minutes as possible.

## Why not a blind `git bisect`

The user's own description of every variant so far has been "flickers **sometimes**".
A blind bisect trusts each single run's clean/flickers verdict to steer the next
83/2, 83/4, ... commits — but a "clean" run on an intermittent symptom is not proof of
absence, it's just a run where the bug didn't happen to fire. One unlucky "clean"
verdict early in a blind bisect sends the whole search down the wrong half and costs
several more rounds to recover from, silently. So this plan front-loads commit reading
to pick a handful of test points for a *reason*, and treats every "clean" verdict as
provisional until repeated once.

## Ranked suspects

Read in full (`git show <hash>`), not just by subject line. Ordered by how directly the
mechanism explains an *intermittent* flicker in *fullscreen*, independent of VRR/HDR/the
overlay — which is everything we've already ruled out.

### 1. `eb1b304` — BufferMemo: do not assert when a destroyed buffer still has texture references

**The strongest single suspect.** `CBufferMemo::OnBufferDestroyed()` used to `assert(
m_pVulkanTexture->GetRefCount() == 0 )` before unmemoizing a destroyed client buffer.
The commit's own message describes a genuine, unfixed cross-thread race: wlroots frees
a client's previous buffer and fires its destroy signal on the **wlserver thread** the
moment the client commits a new frame, but the **steamcompmgr render thread** can still
hold a live reference to that buffer's texture in an in-flight command buffer. The old
code would abort the instant that race fired (loud, easy to notice — nobody could have
missed it in testing). This commit doesn't fix the race, it just stops crashing on it:
it unmemoizes the buffer unconditionally and continues.

That is a textbook mechanism for a *silent, intermittent, timing-dependent* visual
artifact instead of a crash: whichever texture object the memoizer now associates with
that buffer's key can go stale or be reused while still bound into an in-flight
composite for one frame, exactly the "flickers sometimes" shape the user is describing,
and it fires on **every client buffer commit** — i.e. every frame vkcube presents, on
any backend, VRR or not. Not gated on Steam overlay, DRM, or anything we've excluded.

This is also the one suspect we can test *without* relying on the user's eyes: the
control build here uses `meson setup build/` with no `-Dbuildtype`, i.e. Meson's default
**`debug`** buildtype (confirmed against the existing `upstream` control's
`meson-info/intro-buildoptions.json`: `buildtype=debug`, `b_ndebug=false`), so asserts
are compiled in. Build the commit **before** this fix in that same debug configuration
(`upstream-prebuffermemo`, see below) and the race — if it's really what's firing —
should **abort the process with the exact
`m_pVulkanTexture->GetRefCount() == 0` assertion**, not flicker. A crash is unmissable;
it doesn't need sustained watching the way a flicker does.

### 2. wlroots bumped twice in this window: `4286887` (→0.19) then `fc6a965` (→0.20)

Two full major-version bumps of the library gamescope embeds to host every
client (game) surface as a nested Wayland compositor — buffer commit, damage tracking,
frame-callback scheduling all live there. `4286887` is bigger than a version bump: it
also **switches the wlroots fork gamescope builds against**, from Valve's own patched
fork (`Joshua-Ashton/wlroots.git`) to vanilla upstream
(`gitlab.freedesktop.org/wlroots/wlroots.git`). Whatever gamescope-specific patches
Valve's fork carried for its own buffer-lifecycle/scanout assumptions are gone from
this point forward, replaced wholesale by two versions' worth of unrelated upstream
wlroots changes we have not read (they're not gamescope commits, they're a submodule
pointer bump — reading them is a much bigger undertaking than the 83 gamescope commits
this doc is scoped to). This is a plausible, broad-blast-radius suspect precisely
because it's opaque; rank it second because we can bracket its effect with a build
point (`upstream-wlroots020` below) without having to read wlroots' own history.

### 3. `6ab4a7d` — rendervulkan: bound FrameInfo_t layers behind a stack

A large mechanical refactor (195 insertions / 159 deletions across
`DRMBackend.cpp`, `OpenVRBackend.cpp`, `WaylandBackend.cpp`, `rendervulkan.cpp`,
`rendervulkan.hpp`, `steamcompmgr.cpp`) replacing every direct
`frameInfo->layers[i]` / `frameInfo->layerCount` access with a bounds-checked
`LayerStack_t` (`push()`/`pop()`/`count()`/`get()`/`truncate()`). Motivated by a real
buffer-overflow fix (a frame with enough planes could write past the fixed array), but
a refactor this wide, touching **every** producer and consumer of the per-frame layer
list across every backend in one commit, is exactly the shape of change that can shift
an off-by-one in `truncate()`/`pop()` ordering and leave a stale or wrong layer in the
stack for one frame — which reads as a one-frame visual glitch, i.e. a flicker. Lower
confidence than #1 and #2 because it's a mechanical conversion rather than a described
behavioural change, but it is in the direct compositing hot path for every backend
(including SDL), so it stays in the top three.

### Considered and deprioritized

- **`25f929e` DRMBackend: Carry pending mode to composite path when direct scanout
  fails** — fits "direct scanout eligibility" and "compositing bypass" from the task
  brief almost too well, but it's in `DRMBackend.cpp`. The user's reproduction (and
  this harness's `upstream`/`normal` variants) run `--backend sdl`, nested under
  Hyprland — gamescope's own `DRMBackend` class is never instantiated on that path; the
  *host* compositor owns the real KMS/DRM state. Worth reopening only if the user later
  reproduces the flicker running gamescope as the primary DRM output directly (no
  nesting) — see the note in "Reading the results" below.
- **`9dad5bd` wlserver: Fix use-after-free retiring a destroyed gamescope_swapchain** —
  a real UAF, but scoped to Steam overlay "content override" windows registered before
  their X11 window is known, and the crash it fixes only fires on client/server
  teardown (window close), not steady-state presentation. `vkcube` fullscreen doesn't
  exercise this path.
- **`8b206cb` layer: perform present queue operations for retired swapchains** — only
  matters when a swapchain is retired (resize / mode change mid-run). The test client
  runs at a fixed resolution the whole time.
- **`6645037` Use a backing surface for letter-boxing** — 5-line change, gated on
  `--force-grab-cursor`, and in `WaylandBackend.cpp` (gamescope's `--backend wayland`),
  not `SDLBackend.cpp`. Wrong backend for our reproduction.

## What upstream already knows

Checked via `gh` against `ValveSoftware/gamescope` (issues + merged PRs), and confirmed
our local partial clone's `origin/master` is **currently sitting exactly at `fcc1341`**
— i.e. there is nothing merged upstream *after* our base commit yet to cherry-pick.

- **No existing issue matches this bug.** Searched `flicker`, `flicker fullscreen`,
  `scanout tearing`, `BufferMemo`/`texture reference`/`corruption`. Nothing describes
  1920x1080, the SDL/nested backend, or a post-3.16.24 fullscreen flicker specifically.
- **Closest partial match, and why it's not this bug:** issue **#2309** ("[NVIDIA/DRM]
  Scan-out corruption at every 4K mode, clean at 1080p/1440p; wlroots unaffected on
  identical DRM state", opened 2026-08-05, still open, 3 comments) describes
  intermittent (~50% per boot) scan-out corruption — but explicitly reports **1080p as
  clean** in their own resolution-vs-pixel-clock table, and it's the DRM/NVIDIA
  embedded backend, not the SDL/nested backend. Different bug, but confirms gamescope
  has had more than one live scan-out corruption issue in exactly this period, and that
  intermittency (their own words: "please treat any fix as needing several boots") is
  an established pattern for this class of bug upstream, not just something specific to
  this user's hardware.
- Issue **#2140** ("SDL nested surface has obvious corruption at the bottom of a 4K
  screen") is the only other SDL-backend corruption report, but predates our window
  (filed against `3.16.23+`), has zero comments/fix, and its trigger (scrolling Big
  Picture at 4K) doesn't match a static fullscreen `vkcube` client at 1080p.
- **No fix to cherry-pick exists.** Since `fcc1341` is upstream's current tip, there is
  no later commit to pull in — whatever this is, we would be the first to isolate it.

## Pre-built binaries

All built the same way as the existing `upstream` control:
`meson setup build/ -Denable_tests=false && ninja -C build` (Meson's default
`debug` buildtype, matching `upstream` — this matters for suspect #1's assert). Each
lives in its own `git worktree`, sibling to this repo, and is wired into
`scripts/flicker-ab-test.sh` as a selectable variant (see `--list`):

| Variant | Commit | What it brackets |
|---|---|---|
| `upstream` | `fcc1341` (HEAD) | known bad — flickers |
| `upstream-wlroots020` | `fc6a965` | everything through the wlroots 0.20 bump and the BufferMemo fix; excludes only the final 16 commits (dominated by `6ab4a7d` and steamcompmgr focus-window churn) |
| `upstream-prebuffermemo` | `9ab4ace` (`eb1b304`'s parent) | everything before the BufferMemo assert removal — debug build, so this should **crash, not flicker**, if suspect #1 is real |
| `upstream-3.16.25` | `17baf4a` (the `3.16.25` tag) | first 20 commits after `3.16.24`; all three ranked suspects live *after* this point |
| (baseline) `3.16.24` | — | known good — the user's packaged binary; not rebuilt, already established clean |

Worktree paths (siblings of this repo, matching the existing `../gamescope-upstream-fcc1341` pattern):
`../gamescope-upstream-3.16.25`, `../gamescope-upstream-prebuffermemo`,
`../gamescope-upstream-wlroots020`. Override with
`GAMESCOPE_RITZ_AB_UPSTREAM_31625_BIN` / `_PREBUFFERMEMO_BIN` / `_WLROOTS020_BIN` if
you built them elsewhere — see the script's `--help`.

## Accounting for intermittency

Every "clean" verdict below is provisional, not final:

- **Watch each variant for at least 60 seconds** of active fullscreen before calling it
  clean — the flicker is intermittent, and a 5-second glance is not enough exposure to
  trust a "didn't see it" result. If you can leave `vkcube` spinning for 2-3 minutes
  while doing something else nearby, even better.
- **Any "clean" result that would end the search or flip your leading hypothesis must
  be reproduced with a second run before you trust it.** Two independent clean runs of
  the same variant is a much stronger signal than one. A single "flickers" result,
  conversely, does **not** need repeating — the flicker is a real, positive event; you
  can't "false-positive" your way into seeing something that isn't there.
- Run each variant back-to-back with `upstream` (the known-flickering fcc1341 control)
  on the same session before drawing conclusions from it, the same way the original
  A/B plan anchored every run against `normal`. If `upstream` itself somehow comes out
  clean on a given attempt, distrust that whole session and start the pair over.

## Test order

Four commands, run in this order, each preceded (or followed) by a re-run of
`upstream` as the anchor per the note above:

```
scripts/flicker-ab-test.sh upstream-3.16.25
scripts/flicker-ab-test.sh upstream-prebuffermemo
scripts/flicker-ab-test.sh upstream-wlroots020
scripts/flicker-ab-test.sh upstream
```

`upstream-prebuffermemo` is ordered second, ahead of `upstream-wlroots020`, even though
it's chronologically later in the commit range than `3.16.25`'s midpoint would suggest
bisecting next — because it is the one test with an **unambiguous, low-attention
signal** (crash vs. no crash) rather than another "watch and squint" flicker call. If it
crashes, you have your answer in under a minute and can stop.

### Reading the results

Work through outcomes in this order — each answers a specific question:

1. **`upstream-prebuffermemo` aborts with the `BufferMemo.cpp` assertion** → suspect #1
   confirmed as *a* live race in this exact build; strong evidence it's also the
   flicker's mechanism in the post-fix (`eb1b304`+) builds, since a race that corrupts
   texture state under load is a very natural source of an intermittent visual glitch.
   Report this back — from here the fix is understanding *why* the reference outlives
   the destroy (a pending render command buffer, per the commit message) and whether
   gamescope-ritz should hold a stronger reference/fence instead of relying on
   `BufferMemo` unmemoizing safely.
2. **`upstream-3.16.25` flickers** → all three ranked suspects are still *possible*
   (none of them exist yet at this tag), but the regression is actually in the smaller
   **20-commit** range `3.16.24..3.16.25` instead — re-read that shorter list (already
   enumerated in this doc's git history search) with fresh eyes; the SDL-backend and
   pipewire/vblank-adjacent commits there (`b3ecd00`, `16a44df`) are the next things to
   read closely.
3. **`upstream-3.16.25` clean, `upstream-wlroots020` clean, `upstream-prebuffermemo`
   clean (no crash) or flickers instead of crashing** → the regression is in the final
   16-commit tail (`6ab4a7d`..`fcc1341`), which makes suspect #3
   (the layer-stack refactor) the leading candidate almost by elimination. Worth a
   direct bisect of just those 16 commits at that point — a small, cheap range.
4. **Everything before `upstream` (i.e. `fcc1341`) is clean, `upstream` itself
   flickers** → the regression is in the very last handful of commits
   (`fc6a965`..`fcc1341`, the 16-commit tail), same conclusion as (3) reached from the
   other direction — corroborating evidence, not a new branch.

## The rebase-to-3.16.24 fallback: assessed, not recommended

The alternative to finding the exact regressive commit is simpler on paper: rebase this
fork onto the `3.16.24` tag (empirically known clean) instead of `fcc1341`, and give up
the 83 commits' worth of upstream fixes between the two. This was actually attempted —
not just estimated — as a throwaway `git rebase --onto <3.16.24> fcc1341 <our-tip>` in a
disposable local branch (discarded after, nothing pushed or merged).

**Verdict: viable but not cheap, and not a trap of the "silently wrong" kind — the
build system stops you immediately, in a way you can't miss.**

- The rebase **does not apply cleanly**. It conflicts on only the **7th** of 53
  linearized commits, in `tests/meson.build`: upstream itself restructured its test
  target between `3.16.24` and `fcc1341` (consolidating `test_convar` into one
  `gamescope_tests` executable covering multiple suites), and our own
  `config-system` test addition (`d5b311a`) was written against the *old* single-test
  layout. This is a real structural conflict, not a one-line whitespace nit — it needs
  a person to decide how the merged test target should look, not just `git checkout
  --theirs`.
- **`VK_KHR_dynamic_rendering` / `CVulkanDevice` queue setup — the specific thing we
  were asked to check — is fine.** Our fix (`d8b5d88`) only touches
  `CVulkanDevice::createDevice()`'s extension list, `CVulkanCmdBuffer`'s
  constructor/destructor, and adds `generalCommandBuffer()` — none of that code is
  touched by any of the 83 commits in the flicker-regression window (they touch
  `rendervulkan.cpp` in *different* areas — layer/backend composition, DRM format
  checks, screenshot handling, HDR metadata gating — not device creation or command
  buffer pooling). This specific dependency is **not** a blocker for rebasing.
- **The bigger cost is elsewhere: `wlserver.cpp` and `steamcompmgr.cpp` are both hot in
  this exact commit window.** Roughly ten of the 83 commits touch `wlserver.cpp`
  (hotkey-key tracking rewritten three times, a use-after-free fix, cursor-freeze fix,
  several destructor/listener cleanups) and a similar count touch `steamcompmgr.cpp`
  (focus-window handling, override painting, screenshot fixes). Our own overlay work
  touches both files substantially (237 and 54 changed lines respectively, including
  the keyboard/pointer input-capture commit). The one conflict actually hit in the
  59-second `tests/meson.build` collision is very likely not the only one waiting in
  those two files — the throwaway rebase was aborted right after the first conflict to
  avoid spending more of this session on a path not yet chosen, not because it looked
  unrecoverable.
- **Net assessment:** this is a real, resolvable afternoon of manual conflict
  resolution (not a multi-day undertaking, and not "silently broken" — every conflict
  stops the rebase and demands a decision), but it is strictly more expensive than
  finding and fixing (or reverting) the one regressive commit once the bisection above
  narrows it down, *provided* the bisection above actually lands on something
  addressable. Recommend treating this as the fallback of last resort: run the four
  tests above first. If they converge cleanly on suspect #1 or #3, fixing/reverting
  that single commit against `fcc1341` is cheaper than resolving ~10-20 commits' worth
  of conflicts against `3.16.24`. Only reach for the rebase if the bisection above comes
  back inconclusive after the confirm-clean-twice discipline is actually followed.
