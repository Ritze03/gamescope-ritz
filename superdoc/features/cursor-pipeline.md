# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread).

**By default, gamescope draws a pointer only for its own settings overlay.** That
pointer is a plain triangle, accent-coloured outline, black inlay, defined in
`src/Overlay/CursorArt.cpp`. **While the overlay is closed and the Cursor tab's
"Use everywhere" toggle is off (the default) gamescope does not touch the cursor at
all** -- the game keeps whatever it or the host set, exactly as upstream does.
Getting to that took three attempts; see "Three attempts at the overlay pointer"
below for what each cost -- and see "Use everywhere: reinstating the game-side
pointer as an opt-in" further down for why a fourth attempt, now opt-in only, was
added on top of that history rather than replacing it.


## Sub-pages

| Page | Covers |
| --- | --- |
| [Cursor pipeline: sources and threading](cursor-pipeline-sources-and-threading.md) | Three cursor sources, why default/live-system cursors are not substitutes, threading rule |
| [Cursor pipeline: the overlay pointer and "Use everywhere"](cursor-pipeline-overlay-pointer.md) | Overlay pointer, Cursor tab, three attempts, Use everywhere, Override game cursor |
| [Cursor pipeline: the overlay freeze](cursor-pipeline-freeze.md) | The overlay freeze and the FPS HUD follow-up |
| [Cursor pipeline: doubled pointer speed and the absolute notify](cursor-pipeline-pointer-speed.md) | Doubled pointer speed, unconditional absolute notify |
| [Cursor pipeline: hotspot, stretched resolution and oversized windows](cursor-pipeline-hotspot-and-geometry.md) | Hotspot fix, stretched resolution, window larger than the screen |
| [Cursor pipeline: locked pointer and the SDL3 device](cursor-pipeline-locked-pointer.md) | Locked pointer never gets an absolute event, the device SDL3 remembers |
