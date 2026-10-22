# gamescope-ritz

A fork of Valve's [gamescope](https://github.com/ValveSoftware/gamescope), a Wayland
micro-compositor for gaming that runs games nested in a window or as the primary
DRM/KMS output on a display. Written in C++, built with Meson.

This repo's base commit is exactly upstream `ValveSoftware/gamescope` HEAD (`fcc1341`)
— the recent OpenVR-backend and steamcompmgr focus-handling commits are Valve's own
upstream work, not this fork's divergence. This fork's actual own work is additive on
top: the settings-overlay/FPS-HUD system (see `superdoc/planning/
overlay-presentation-architecture.md`).

<!-- superdoc:start v2 -->
## Docs: read before you touch, update after you change

Full discipline — what to read, when to update, how to record the *why* — lives in the
always-loaded `superdoc/claude-instructions/documentation.md` below; this is a pointer, not
a restatement.

Reference docs — plain links, read the one relevant to your task on demand:

- **`superdoc/architecture/overview.md`** — codebase navigation map (module map, data flow,
  "where to look for X"). **Start here to navigate the code.**
- Per-feature behaviour — `superdoc/features/`
- `superdoc/README.md` — the docs table of contents
- **Building (agents)** — build and test ONLY with `scripts/gentle-build.sh` (build dir
  `build-agent/`) — never a raw `ninja`/`meson compile`, never an LTO `build-release/`
  build — unless the task is specifically about the release/install build itself. See
  `scripts/README.md` and `superdoc/features/build-and-tooling.md`.

- **Live audio tests: listen, don't play; 1% volume, hard cap.** When audio is already
  running on the system, check for a signal by *reading* it (e.g. `pw-record` from a
  monitor port) instead of playing anything — the user: *"just use the systems default
  audio, no need to play something, if something is running"*. Any test stream an agent
  must start plays at
  **1% volume or less** (`pw-cat --volume 0.01`, a sink/stream volume of 0.01 set before
  any sample flows) — "it's only silence" is no exception. Never raise, reset or re-route the
  volume of the user's own streams, sinks or devices, and never move the user's own streams.
  `Why:` a 2026-10-07 audio spike played "silence" at full volume and blasted the user; the
  user's own rule: *"1% volume is enough, to see a signal"*.
- **Banned name in tracked files.** Never write the four-letter abbreviation of Lossless
  Scaling Frame Generation (any case, also as part of the Vulkan layer's name) in anything
  synced to GitHub — docs, code comments, commit messages, any tracked file. Describe
  frame-gen-ritz's Motion version 2 as a "morphing" interpolator. `Why:` the user's explicit
  rule (2026-10-22).

## Always in context (force-loaded, mandatory)

`@` is reserved for the must-always-know set. Everything above is a plain link: an
optional, on-demand read.

- **Terminology** — @superdoc/meta/TERMINOLOGY.md — project vocabulary; use these meanings,
  ask before acting on an undefined term, and keep it current.
- **Documentation discipline** — @superdoc/claude-instructions/documentation.md — read
  before you touch, update after you change, record design rationale (the *why*).
- **Version policy** — @superdoc/claude-instructions/documentation-version-policy.md — how
  this project bumps doc/version markers.
- **Changelog maintenance** — @superdoc/claude-instructions/changelog.md — `CHANGELOG.md`
  (repo root) gets a one-line entry, in the **same commit**, for every user-visible change.
  Strict format, four categories, one version bump per dated block. The overlay parses
  this file, and the build reads the top block's version as the project's own — so
  `CHANGELOG.md` **is** the version marker; there is no other.

Path base: `CLAUDE.md` uses repo-root-relative paths (`@superdoc/...`, `` `superdoc/...` ``);
links *inside* docs are docs-relative. Precedence: `CLAUDE.md` is authoritative for
rules, docs are the reference — reconcile to `CLAUDE.md` on conflict. `@`-budget:
force-load a file only if its absence would let the agent do the wrong thing on *any*
task.
<!-- superdoc:end -->
