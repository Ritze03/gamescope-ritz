# Command palette and Launcher — every setting, one keystroke away

The **Command palette** is a searchable, ranked index over every registered setting (`Entry`) and
every parameter (`Parameter`) in the overlay registry. Drawn over an open **Shell** it is the
palette; drawn **alone over the game**, with nothing behind it, it is the **Launcher** (the user's
"Launcher Style UI"; "Launcher-UI" is the same thing). Mid-game you usually want one setting, not a
tour of the settings surface, and the Launcher exists so a keystroke gets you exactly that. The
framework the palette indexes is documented in [overlay-shell.md](overlay-shell.md); the hotkey that
opens it in [keybinds.md](keybinds.md).

## Files

| File | Role |
| --- | --- |
| `src/Overlay/UI/CommandPalette.{h,cpp}` | The pure half: `Build()`, `Score()`, `PaletteItem`, the score bands, UTF-8 query editing helpers, `WorstCharsToReach()`. ImGui-free. |
| `src/Overlay/UI/Shell.cpp` | The drawn half: `OpenPalette()`, `DrawPalette()`, `PaletteConsumeInput()`, `RunPaletteKeyboard()`, `PaletteJump()`, `PaletteItemAdjustable()`, and the launcher branch of `Draw()`. |
| `src/Overlay/UI/Layout.{h,cpp}` | `SolvePalettePanel()` and the `shelltok::kPalette*` metrics. |
| `src/Overlay/UI/Shell.h` | The thread-safe request API: `RequestPalette()`, `RequestLauncher()`, `RequestClosePalette()`, `RequestLauncherClosePreservingQuery()`, `LauncherOnlyActive()`, `PaletteActive()`, `NotifyOverlayHidden()`. |
| `src/wlserver.cpp` | `wlserver_check_ritz_keybinds()`, `case Action::Launcher`: what the hotkey does. |
| `tests/test_overlay_ui.cpp` | Scoring, ranking, the discoverability test, `SolvePalettePanel()`. |

`src/Overlay/Palette.{h,cpp}` is **not** this: it is the colour-token palette (`gamescope::palette`).

## How matching and ranking work

- **Pure and window-free.** `Score()` takes a title, an id, a lowercased haystack and a lowercased
  query and returns an integer band; `Build( Registry, query )` walks every available area and
  returns a stable-sorted `std::vector<PaletteItem>`. Neither touches ImGui, so the tests exercise
  them with no compositor.
- **Bands (low is better)**: `kScoreExact` (title starts with the query, and the empty query),
  `kScoreTitle` (title contains it), `kScoreId` (config key / id contains it), `kScoreKeyword`
  (the title + id + keywords blob contains it), `kScoreFuzzy` (characters appear in order),
  `kScoreNoMatch` (filtered out).
- **Browsing is search with an empty query.** `Score()` returns `kScoreExact` for an empty query, so
  every item matches at equal rank and the *stable* sort keeps registration order, which is rail and
  sheet order. `Why:` there is deliberately no second "list everything" path; one code path is what
  stops the browse list and the search list drifting apart.
- **Params are searchable exactly like rows**, labelled `Parent » Param` (U+00BB, not the mockup's
  U+25B8: `Fonts.cpp` bakes Basic Latin + Latin-1 only and anything else draws as the fallback box).
  A Param's haystack includes its parent's title and keywords, so "hdr denoise" finds a Param called
  Denoise under HDR: users search by where they remember a setting living, not just its name. `Why`
  params are indexed at all: this loop is the whole of "a setting in the Inspector is one keystroke
  from anywhere", and the reason the Param budget law is credible rather than merely strict.
- **What is excluded**: areas that are not `Available()` (a palette route to a row the rail
  deliberately hides is the "reachable but not present" state); read-only entries (`Meter`, `Facts`,
  `Graph`: nothing a jump could act on); entries marked `HideFromPalette()` (issue #91: a setter
  exists but a jump from the Launcher makes no sense). Exclusion hides the entry **and its params**.
- **The path column shows the config key**, not the area id (`PaletteItem::sPath`, decision D5).
  `Why:` an area id is a UI grouping label with no promise that the settings inside share it as a
  key prefix (four of eleven areas did not at the time); the key is what a reviewer checks against
  the on-disk JSON.
- **Discoverability test.** `WorstCharsToReach( reg, nTopN, &id )` returns the fewest leading title
  characters that bring the worst-case setting into the top N. Direction B gated its build on this
  (3 characters or fewer); here it ships as a **test**, not a registration abort (decision D16).
- Lowercasing is ASCII only (`ToLowerAscii()`): ids and keyword blobs are ASCII by construction and a
  non-ASCII title still matches through its id, so a Unicode fold would cost a dependency for nothing.

## The query field is hand-rolled

The query is not `ImGui::InputText`; `PaletteConsumeInput()` reads `io.InputQueueCharacters`
directly into `s_sPaletteQuery` (`AppendUtf8()`, `PopUtf8()` for Backspace, `PopWord()` for Ctrl+Backspace). `Why:`
- `InputText` owns Left/Right with no way to decline them, and the palette needs them to **adjust
  the highlighted entry's value in place**;
- its Esc reverts the buffer instead of dismissing the palette;
- it buys no IME here anyway: this ImGui context has no platform backend (custom Wayland input,
  `ImGui_ImplVulkan` for rendering), so the IME-position callback is null either way.

What is lost (selection, caret placement, paste, dead keys) is acceptable for a search box a couple
of words long. wlserver fills the queue from the compositor's own xkb state, so the text arrives as
layout-correct UTF-8. The palette is the sole consumer while open and clears the queue. Any edit
resets the highlight to the top: the old index would otherwise point at a different setting, and an
arrow key after typing could silently adjust the wrong row.

## What the keys do (`RunPaletteKeyboard`)

- **Up/Down** move the highlight. **Left/Right** adjust the highlighted entry **in place**
  (Shift = fine step) through `AdjustValue( Adjustable::Of(...) )`, the same function the sheet's
  arrow keys use, so a value cannot step differently in the two hosts.
- **Enter** is `PaletteJump()`: select the row in the shell (`SetSelectedArea()` + `Select()`).
  A Param opens the Inspector in Configure; if the Inspector is Hidden the host is promoted to
  Drawer (Reachability Law). In the **Launcher**, Enter also **promotes it to the full shell** at
  that row (`s_bLauncherOnly = false`; the layer stays up). `Why:` this answers "what happens when a
  result cannot be adjusted in place" (a composite, a text field, a bank: their controls do not fit
  a 38 px result row and the Launcher does not grow one). It costs the Launcher's purity, which is
  the right trade: the user's complaint was that the *keybind* dragged the shell in unasked, and an
  Enter on a row they chose is not that. Showing a result you cannot act on would turn a dead row
  into a puzzle.
- **Esc** closes the palette only. Over a shell the shell stays; in the Launcher Esc **gives the game
  back** (the launcher branch notices `!s_bPaletteOpen` and calls `CloseShell()`), and so does a
  click on the slab outside the panel. The footer legend is per highlighted row, never generic: *left/right
  adjust in place* appears only for a row `PaletteItemAdjustable()` says can be adjusted (otherwise
  *Enter open in the full overlay*), and Esc reads "back to the game" in the Launcher, "dismiss" over
  a shell. `Why:` a fixed legend promising an adjust over a row that cannot adjust is an
  instruction that does nothing, the same defect class as a control that renders and does nothing.
- **Ctrl+K** opens the palette from inside the shell (checked first, so it also works while the
  palette is open; reopening clears the query, as every palette in every editor does).
- A click-guard: the pointer position is `-FLT_MAX` until the first motion event and counts as
  "outside the panel" for any rect test, so without a check the first click of a session dismissed
  the Launcher wherever it was aimed (found in the acceptance run).
- At most 60 rows are built for drawing (the answer to a long list is a better query, and it bounds
  per-frame cost); 9 are visible (`shelltok::kPaletteRowCap`).

## The Launcher binding

The `launcher` **keybind action** (default `LCtrl+RShift`, editable in Keybinds; handled in
`wlserver.cpp`'s `case Action::Launcher`) is a **toggle**. Pressed again while the palette is
already up, it closes what it opened rather than opening something else:

| State when pressed | Result |
| --- | --- |
| Nothing open | Overlay layer on (`SettingsOverlay_SetVisible( true )`), then `RequestLauncher()` (alone, over the game). |
| Shell already open | `RequestPalette()`: the palette laid over the shell. |
| Launcher alone is up (`PaletteActive()` and `LauncherOnlyActive()`) | `RequestLauncherClosePreservingQuery()` then hide the whole overlay. |
| Palette over a shell the user opened separately | `RequestClosePalette()` only; the shell is untouched. |

`Why` `SetVisible`, not toggle: toggling would close the overlay in the common case where the
shortcut is used while it is open. `Why` the toggle (issue #88): a binding that can only open leaves
the user reaching for Esc; and without `LauncherOnlyActive()` a second press while the Launcher was
up would read as "the shell is open" and summon the shell, because `settings_overlay_visible` is true
in both cases. Hiding the overlay on the shell-over-palette path would blank a shell the combo
never opened.

**Query preservation.** The user's wording (2026-08-29): *"Closing the Launcher with CtrlL and CtrlR
should not empty the search field. Closing it with RCtrl only or Escape should empty the search,
like it does now."* So only the launcher binding's own close keeps the query
(`RequestLauncherClosePreservingQuery()` stores one atomic bit that `OpenPalette()` consumes on the
thread that owns the string); the shell binding's tap and Esc do not ask for it and clear on the
next open, as before. The bit is stored rather than the text because the text is only ever touched
by the draw thread.

**Esc must never flash the shell** (the D31 flash fix). The user: *"When i open the Launcher Style
settings (both CTRLs), and close it with Escape, the GUI shows up for a split second."* Hiding the
overlay does not stop it drawing: `SettingsOverlay.cpp` fades the layer over `k_uOverlayFadeMs`
(200 ms) and draws while `s_flCurrentAlpha > 0`, about 12 more frames, and the Esc handler had
cleared `s_bLauncherOnly` before asking for the hide, so every fade frame fell through to the full
shell. The fix is structural, not an ordering: the launcher/shell mode is assigned **only on an
opening edge** (and by `PaletteJump()`, a promotion the user asked for), nothing on a closing path
writes it, and `LauncherOnlyActive()` is *derived* (it reports the mode only while the layer is
capturing), so the stale mode is unobservable and closing cannot change what is drawn. `Why`
structural: two bits (layer up, is launcher) written by different code at different moments leave
a window where the wrong surface draws; reordering two lines only shrinks the window until the next
site forgets. The launcher branch of `Draw()` is an **early return** rather than guards threaded
through the shell's draw: a return cannot be forgotten by a future region.

**The panel never moves while you type.** The user: *"make sure that it is vertically centered, in
its initial state ... do not dynamically move it according to the current height ... So it doesnt
start moving the search bar, all of the sudden"*. `SolvePalettePanel()` centres the panel once, at
its **maximum** row count (`kPaletteRowCap`), and the list grows downward inside a frame whose top
edge never moves. Its inputs are the frame, the scale-derived row metrics and the cap; the match
count is deliberately not an input, so nothing a user types can move the query line. `Why` a pure
function and not a cached `y0`: a cache needs an invalidation rule ("on display_scale and surface
size, not on match count") that is a thing to get wrong later. Where the cap's worth of rows does
not fit (2.0x on a short surface) `nMaxRows` drops to what fits and the panel is centred at that
height; where not even one row fits (`bFits` false) it is top-anchored, because centring a taller
panel pushes the query line off the top and the footer is the cheaper loss.

Dividers inside the panel inset by one `Hairline()` so they do not double-composite on the panel
border (the "frame rule", see [../ui/STYLING-GUIDE.md](../ui/STYLING-GUIDE.md)).

## Binding history

- **D22 (2026-08)**: the palette's binding moved **out of the shell's own keyboard map and into
  wlserver's hotkey table**, as a modifier chord, because the palette had to be reachable with the
  shell closed. Ctrl+K inside an open shell stayed. That chord was **Left Ctrl + Right Ctrl**.
- **D25 / D31 (2026-08-24)**: the Launcher became its own mode (alone over the game, Esc goes back)
  instead of the binding opening the whole shell with the palette on top; D31 fixed two Launcher
  defects (the moving panel and the Esc flash, both above). It is direction B's contribution kept as
  a *feature* rather than the whole GUI.
- **Issue #88 (2026-08-27)**: the binding became a toggle (table above); **item 2 (2026-08-29)**:
  closing with the chord keeps the query.
- **2026-09-01**: the overlay's hotkeys moved off Ctrl. The shell opens with **Right Shift** (tap)
  instead of Right Ctrl; the Launcher with **Left Ctrl + Right Shift** instead of Left Ctrl +
  Right Ctrl. Right Ctrl no longer does anything alone. Two follow-ups: Ctrl+Shift+O stopped
  working with the right-hand Shift (the combo opened the Launcher the moment Shift went down and the
  O then closed it), so both overlay hotkeys now act when Right Shift comes **up**, which means one
  press of the combo can only ever produce one surface; and a modifier stuck "held" after switching
  windows made every later lone Right Ctrl tap open the Launcher instead of the shell.
- **2026-09-08**: both chords became editable **keybind actions** (`launcher` default
  `LCtrl+RShift`, `shell` default `RShift`), see [keybinds.md](keybinds.md).

## Threads

The hotkeys fire on wlserver's thread; shell state is owned by the draw thread. Every request in
`Shell.h` stores one atomic that `Draw()` consumes at the top of the next frame, before anything
draws, so the palette opens on the same frame the request is seen and the shortcut feels like the
key opened it. If an open and a launcher request arrive in one frame the launcher wins (the more
specific ask). The close request is mutually exclusive with both opens by construction: wlserver
decides open-vs-close from `PaletteActive()` before it sends anything. `LauncherOnlyActive()` and
`PaletteActive()` read published atomics.

## Related

- [overlay-shell.md](overlay-shell.md), [keybinds.md](keybinds.md)
- Design record: `superdoc/planning/redesign/AUTONOMOUS-DECISIONS.md` (D16, D22, D25, D31),
  `superdoc/planning/redesign/round-2/e2-inspector-plus/IMPLEMENTATION.md` (issue #88)

## History

- **Launcher** (terminology entry, moved here 2026-11-07): **Launcher** — the command palette drawn **alone over the game**, with no shell behind it (`Left Ctrl + Right Shift` while the overlay is closed — rebound from `Left Ctrl + Right Ctrl` 2026-09-01, see CHANGELOG.md). The user's *"Launcher Style UI"*. Distinct from the palette drawn *over an open shell*, which is the same widget with a scrim and a different Esc. See IMPLEMENTATION.md's Issue #88 entry.
