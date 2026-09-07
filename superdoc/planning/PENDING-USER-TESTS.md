# Pending user tests

This is a plain checklist of the things from the 2026-09-07/08 batch that only make
sense on your own hardware and your own games — everything else was built and verified
headlessly (desktop) and on the test laptop already. Nothing here needs technical
detail back; a thumbs up/down and a sentence per line is plenty. Once you've gone
through this list, tell your assistant the results — answering it lets the temporary
`@`-reference to this file in `CLAUDE.md` be deleted, and this file deleted with it.

- [ ] **Rust, changing the resolution mid-game.** In a menu, pick a game resolution
  smaller than Rust's own, close the overlay, then move the mouse into the right and
  bottom quarter of the picture: does the cursor reach every corner, and does a click
  land where the cursor is? Try a larger resolution too, and in a match (mouse look)
  confirm the view doesn't jump or drift when you change it.
- [ ] **CS2, a full match, as a mouse regression check.** Menus, in-match look, fast
  stops, a mid-match resolution change — nothing here should feel different from
  before, since this batch didn't touch the CS2 mouse path itself.
- [ ] **Adaptive Brightness Dynamic on a dark and a bright CS2 workshop map, and Rust
  at night.** Is it actually better? Does anything still pulse (breathe brighter/darker)
  or look blown out? Try Local adaptation at 0, 50 and 100 on each and say which you'd
  actually leave it on.
- [ ] **Are Max gain 4.0 and Min gain 0.3 the right defaults now?** Widening them lifts
  dark scenes a lot further, but on a bright scene the honest trade-off measured here is
  that highlights come out slightly darker with deeper shadows than the old 0.5/2.0
  defaults. Worth it, or should the defaults move back partway?
- [ ] **Crosshair gap at 1 px and 2 px.** Gap 1 should look like a solid plus with
  exactly one pixel missing at dead centre; gap 2 should open a small, visibly
  *symmetric* gap with no stagger between the two arms. Also check Apply Scaling at
  your native resolution (nested = output, no stretch) — it measured as an exact no-op
  here; if it still visibly stretches for you, note your build's commit hash and
  whether the Resolution area's live nested/output line reads as equal.
- [ ] **The before/after preview strip on Adaptive Brightness.** Is the small
  before/after picture actually useful for judging a setting? And is it a problem that
  it scrolls away with the sliders on a shorter screen (at 1280x720 it's off the top of
  the page by the time you reach Min gain)?
- [ ] **Appearance and Cursor settings surviving a restart.** Change something in
  Appearance (accent colour, overlay scale, blur, opacity), then something in Cursor,
  then close and reopen gamescope-ritz — confirm both changes are still there and
  neither undid the other.
- [ ] **The new HUD number colour and text opacity rows.** With Text colour set to
  Fixed: turn on Number colour and pick one — the FPS digits should switch to it
  instead of following your UI accent. Then lower Text opacity from 1.0 — the digits
  should visibly fade toward the background.
