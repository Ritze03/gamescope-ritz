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
  Strict format, four categories, no version numbers: the overlay parses this file.

Path base: `CLAUDE.md` uses repo-root-relative paths (`@superdoc/...`, `` `superdoc/...` ``);
links *inside* docs are docs-relative. Precedence: `CLAUDE.md` is authoritative for
rules, docs are the reference — reconcile to `CLAUDE.md` on conflict. `@`-budget:
force-load a file only if its absence would let the agent do the wrong thing on *any*
task.
<!-- superdoc:end -->
