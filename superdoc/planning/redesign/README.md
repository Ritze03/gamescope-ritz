# Overlay redesign proposals

Preserved design explorations for the settings overlay. **Nothing here is implemented.**
These are kept so a later regression can be rolled back against a known-good design
intent, not just against code.

Five directions were explored independently on 2026-08-23; three were kept.

| | Direction | Core idea |
|---|---|---|
| **A** | [`a-console/`](a-console/) | Delete the window manager. One full-bleed pane, a rail of sections, a stage. Depth comes from drilling into a row, never from a second window. |
| **B** | [`b-command/`](b-command/) | Every setting registers once as a searchable entry; that one registry drives search, browse and rendering. **Kept as a *feature*, not as the whole UI** — the command palette, not the navigation model. |
| **E** | [`e-inspector/`](e-inspector/) | One fixed slab in three regions that never move: a rail of categories, a sheet of settings, and an inspector that always describes the selection. |

Each folder holds:

- `index.html` — self-contained interactive mockup, real setting names and values
- `SPEC.md` — navigation, control taxonomy, theming
- `API.md` — the proposed helper layer, with C++ call sites
- `FEASIBILITY.md` — honest assessment against immediate-mode ImGui, plus migration cost

## Decisions recorded

**Why a helper layer at all.** The user's goal, in their words: *"what i want is basically our
own framework/helper functions (on top of ImGui), that makes it easier for you/AI, to update and
extend the UI, while keeping it consistent with the rest."* Every proposal was judged on whether
adding a setting is one obvious call, and whether producing something inconsistent is *hard*.

**All five directions independently converged** on three things, which is worth treating as
settled regardless of which shape wins: a single row grammar; colour *roles* rather than hex at
call sites; and an API where no public function accepts a pixel, size, colour or font — the
helper owns layout entirely.

**Gamepad support is out of scope.** Dropped by the user on 2026-08-23. Note the finding that
prompted it: gamescope has no gamepad input path today — libinput ignores gamepads, and the
overlay enables keyboard nav only, so any controller-facing design needed a new evdev reader
plus Steam Input integration.

**B is a feature, not the GUI.** The registry idea is portable: any navigation model can adopt
registration-driven settings and a command palette without adopting search-first navigation.

## Directions not kept

Two further directions (a tiled workspace, and a gamepad-first radial HUD) were explored and
dropped. One measurement from the tiling exploration is worth keeping even though the direction
was not: of 41 fix commits touching `src/Overlay/`, **16 — 39% — were pure floating-window
management, and nine of those sixteen were fixes to earlier fixes.** That is the strongest
available argument for removing free-floating windows, whichever replacement is chosen.
