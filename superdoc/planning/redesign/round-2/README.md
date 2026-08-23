# Round two — six refinements

Six refinements of directions [A](../a-console/) and [E](../e-inspector/), each built from the
user's critique by an independent agent. **Nothing here is implemented.** Preserved so a
regression can be rolled back against stated design intent.

**E2 (`e2-inspector-plus/`, "Depth on Demand") was chosen** on 2026-08-23 as the direction to
carry forward. The other five are kept for reference and for ideas worth stealing.

| | Proposal | Core move |
|---|---|---|
| A1 | [`a1-refined/`](a1-refined/) | Every critique point fixed at system level. Shipped its own contrast generator. |
| A2 | [`a2-depth/`](a2-depth/) | The Ledge — depth gets a permanent home in the bar freed by dropping gamepad, for zero new pixels. |
| A3 | [`a3-creative/`](a3-creative/) | Bloom — rows are bare at rest; the focused row blooms into a full-size instrument. |
| E1 | [`e1-refined/`](e1-refined/) | The Rail Rule. Diagnosed *why* alignment looked random: the label column pinned control *left* edges. |
| **E2** | [**`e2-inspector-plus/`**](e2-inspector-plus/) | **Chosen.** Everything explanatory leaves the sheet: 76 text lines → 31, zero settings removed. The Inspector has no authoring API. |
| E3 | [`e3-creative/`](e3-creative/) | The Bench — the Inspector becomes a live instrument; the anchor grid becomes a draggable miniature of the real output. |

## Findings worth keeping regardless of direction

**The contrast complaint was real, in every case.** All six agents measured failures in the
design they were handed: meta text 4.03–4.35:1, disabled rows as low as 2.09:1, switch borders
at 1.68:1 — against floors of 4.5:1 for body text and 3:1 for UI. This is the fourth readability
issue on the project (#44, the bottom bar, #62, and #82 for the shipped HUD backdrop default).

**Two independent agents invented the same mechanism.** A3 and E3 both arrived at "hold a key,
the UI drops to ~10%, judge the change against the real game frame" without being prompted
toward it, from different starting points.

**Alignment should be unrepresentable, not discouraged.** All three E agents independently
concluded the fix is to remove the *ability* to express alignment at the API level — no
`SameLine`, no `SetCursorPosX`, no alignment parameter — rather than documenting a convention.

## Ideas flagged as worth stealing into E2

- **E3's anchor Stage** — a draggable 16:9 miniature of the real output that replaces the 3×3 grid
  plus two margin sliders, drawn by calling the same geometry the real HUD uses.
- **E1's hidden-inspector visual** — adopted into E2 at the user's request.
- **A1/A2's registration-time `Help()` assert** — makes empty help text impossible rather than unlikely.
