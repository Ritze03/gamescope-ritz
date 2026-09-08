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
- [ ] **Target brightness and Max gain, swept end to end on a dark map (2026-09-08).**
  This is the one you reported as doing nothing above 0.5 / 2.0, and both were genuinely
  inert on a realistic frame. Adaptive Brightness on, Mode = Dynamic, Local adaptation 0
  for this test. Drag **Target brightness** from 0.1 to 0.9 and say where it stops
  changing the picture; then put it back to 0.5 and drag **Max gain** 1.0 → 4.0 and say
  the same. Two things that are expected and are not the bug coming back: Max gain 1.0
  should now do *nothing at all* (that is the point — it means "do not brighten"), and
  once the picture has reached your Target, further Max gain stops brightening. The
  Shaders area's **Pipeline** row (Details page, "adaptive limit") tells you which limit
  is holding it at any moment — please say whether that line is actually useful or just
  noise. Also: a dark map is now noticeably brighter than before at the same settings —
  is that better, or is it too milky at the defaults?
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
- [ ] **The Ritz extension, in Ritz's own UI (this needed the GUI, couldn't be driven
  headlessly).** Drop `extensions/gamescope-ritz.json` into
  `~/.config/ritz/extensions/` (or accept the installer's offer to do it for you on
  `--install`/`--update`). In Ritz, find "Gamescope Ritz" among your modules and turn on
  its enable toggle — the second field right below it should be a free-text **Profile**
  box (not third, not a dropdown). Type a profile name **with a space in it**, e.g. `My
  Profile`, then use `ritz --print %command%` (or Ritz's own command preview) and check
  the assembled command contains `--profile "My Profile"` as one quoted argument, not
  split into `--profile My Profile` as two separate words. The quoted form is correct;
  two separate words is the bug this pass could only prove by hand-construction, not by
  running Ritz itself — your one test settles it either way.
- [ ] **Vibrancy's direction.** The new Vibrancy boosts a colour's saturation *more*
  the more saturated it already is — that's what you asked for, but it's the inverse of
  what Apple's Photos calls "Vibrance" (which pushes muted colours harder and leaves
  punchy ones alone). Try both it and Saturation and say whether Vibrancy's direction
  feels right, or should be flipped to the Apple sense.
- [ ] **Adaptive Gamma vs. Adaptive Brightness, on a dark map.** Turning one on turns
  the other off — try each on the same dark scene and say which you prefer, and whether
  forcing them to be mutually exclusive is the right call or you'd rather run both.
- [ ] **Bloom, on a real game.** Everything measured for this so far was synthetic
  (flat test fields); try it on something you actually play and say whether it looks
  good, and whether Threshold, Intensity and Radius are the right three knobs or
  something's missing.
- [ ] **Keybinds: rebind something.** Pick a chord under Settings > Keybinds, rebind it
  to something else, and confirm it takes effect immediately with no restart needed —
  then restart gamescope-ritz and confirm the rebind is still there. Also check the
  recovery path still works: `Ctrl+Alt+Shift+O` should always open the settings, even
  if you've rebound everything else to something odd.
- [ ] **Steam chat companion, on `Ctrl+Shift+Tab`.** Is it worth keeping, given it needs
  its own separate login from your Steam client, has no notifications, and costs you a
  browser? The alternative: turning off your Ritz `clear_ld_preload` setting for your
  gamescope titles would restore Steam's own overlay instead — with voice, invites and
  notifications — at the cost of it smearing under frame generation. Which would you
  rather have?
