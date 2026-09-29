# Launch-option lock

When a setting is pinned by a `gamescope` command-line flag at launch, the
matching row in the settings overlay goes read-only for the rest of the
session and grows a small amber **"LAUNCH OPTION"** tag after its label, so
the player never edits a value that a profile switch, a console command, or
the row itself cannot actually change.

`Why:` the user, verbatim — *"Make sure that when a value is overwritten, as
a startup argument, so it's set for the usual GameScope launch arguments,
that it can't be modified inside of the game anymore. Just to avoid
confusion with the user, just add like a small red or yellow label behind it
that warns the user about it being set through a launch argument to avoid
confusion in general."*

## The bit: `src/LaunchOptions.{h,cpp}`

One `enum class Opt` value per lockable setting, plus a tiny store: a bool
and a spelling string per `Opt`, written once by `MarkGiven( Opt, spelling )`
from inside the getopt loop's own `case` for that flag, read everywhere else
through `Given( Opt )` / `Spelling( Opt )`. No reset, no per-profile
override, no clearing — a flag given at launch stays "given" for the whole
process, which is the whole point (the session started with that value fixed
from outside, and nothing in-process is allowed to pretend otherwise).

Write-once, read-many, no lock: `MarkGiven()` is only ever called from the
two getopt loops below, both of which finish (on the main thread) before any
overlay code, any live-apply hook, or any second thread that could read
`Given()`/`Spelling()` exists. This is the same "seed before the readers
exist" contract `main.cpp`'s `g_bForceWindowsFullscreenStartup` already
relies on.

**Spelling caveat.** `getopt_long` collapses a flag's short and long forms
into one `case` (`-w` and `--nested-width` both land in `case 'w':`), so for
an option reached that way `Spelling()` records a canonical `"-w/--nested-
width"` string rather than whichever the player actually typed — `getopt`
doesn't tell the caller which spelling matched. An option reached only
through the `case 0:` long-option branch records the *exact* name
`getopt_long` matched (`gamescope_options[opt_index].name`), since that part
genuinely is known — `--sharpness` and `--fsr-sharpness` report themselves
distinctly, for example.

## The flag → row table

| Flag | Parsed in | `Opt` | Row(s) locked |
|---|---|---|---|
| `-w` / `--nested-width` | `main.cpp`, `case 'w'` | `NestedWidth` | `display.resolution.width`, and `display.resolution.size` (see below) |
| `-h` / `--nested-height` | `main.cpp`, `case 'h'` | `NestedHeight` | `display.resolution.height`, and `display.resolution.size` |
| `-r` / `--nested-refresh` | `main.cpp`, `case 'r'` | `NestedRefresh` | `display.refresh`, `display.refresh.custom` (same config key) |
| `-S` / `--scaler` | `main.cpp`, `case 'S'` | `Scaler` | `display.filter.scaler` |
| `-F` / `--filter` | `main.cpp`, `case 'F'` | `Filter` | `display.filter` |
| `--sharpness` / `--fsr-sharpness` | `main.cpp`, `case 0` | `Sharpness` | `display.filter.sharpness` |
| `-g` / `--grab` | `main.cpp`, `case 'g'` | `GrabKeyboard` | `input.force_grab_keyboard` |
| `--force-grab-cursor` | `main.cpp`, `case 0` | `GrabCursor` | `input.force_grab_cursor` |
| `--force-windows-fullscreen` | `steamcompmgr.cpp`'s own getopt pass, `case 0` | `ForceWindowsFullscreen` | `display.force_windows_fullscreen` |
| `--adaptive-sync` | `main.cpp`, `case 0` | `AdaptiveSync` | `display.adaptive_sync` |
| `--immediate-flips` | `main.cpp`, `case 0` | `ImmediateFlips` | `display.allow_tearing` |
| `--hdr-enabled` / `--hdr-enable` | `steamcompmgr.cpp`, `case 0` | `HdrEnabled` | `display.hdr_enabled` |
| `--framerate-limit` | `steamcompmgr.cpp`, `case 0` | `FramerateLimit` | `display.fps_limit` |

`--force-windows-fullscreen`, `--hdr-enabled` and `--framerate-limit` are
parsed in `steamcompmgr_main()`'s **own** getopt pass (`steamcompmgr.cpp`,
around its `optind = 1` reset), not in `main.cpp`'s — a second, independent
`getopt_long()` walk over the same `argv`, seeded from `gamescope_options`.
`MarkGiven()` is called from inside that loop for those three, at the same
`case` that already sets the flag's own effect.

### Resolution rows: why width/height lock separately, and the size dropdown locks on either

`-w` and `-h` are independent flags that each write one of
`g_nNestedWidth`/`g_nNestedHeight` directly, so `display.resolution.width`
locks on `NestedWidth` alone and `display.resolution.height` locks on
`NestedHeight` alone — giving only `-w` leaves Height still editable, and
vice versa.

`display.resolution.size` (the preset dropdown under Aspect) is different:
one pick sets **both** dimensions in a single `SetSizeChoice()` call, so
locking it only when *both* flags were given would let a lone `-w` be
clobbered by a size pick that also rewrites height. It locks whenever
**either** `NestedWidth` or `NestedHeight` is given.

`-r` sets `g_nNestedRefresh` directly and both `display.refresh` (the preset
choice) and `display.refresh.custom` (the stepper) write the same
`gamescope.nested_refresh_hz` key, so both lock together on `NestedRefresh`.

### `display.fps_limit` and `--framerate-limit`: locked, but the flag's own live effect is separately broken

This is worth stating plainly rather than leaving it to be rediscovered.
`--framerate-limit` writes `g_nSteamCompMgrTargetFPS` directly
(`steamcompmgr.cpp`), a **different** variable from what the settings row
reads and writes (`config.gamescope.fps_limit`, applied through
`steamcompmgr_set_app_refresh_cycle_override()` into
`g_nCombinedAppRefreshCycleOverride[]`). `update_app_target_refresh_cycle()`
runs every frame from `paint_all()` and unconditionally recomputes
`g_nSteamCompMgrTargetFPS` **from** `g_nCombinedAppRefreshCycleOverride[]` —
which the flag never touches — so the flag's direct write is stomped back to
0 on the very next frame regardless of anything this task changed. This is
the same mechanism `PanelDisplay.cpp`'s own long comment on `SetFpsLimit()`
documents as issue #25's bug for the *row's* old (pre-fix) write path; the
flag simply never got the same fix the row did. Locking the row is still
correct — it stops the row from writing a *second*, contradictory value into
`config.gamescope.fps_limit` while `--framerate-limit` was given — but a
player who launches with `--framerate-limit 60` should not expect the cap to
actually take effect today. Fixing the flag itself (routing it through
`steamcompmgr_set_app_refresh_cycle_override()` the way the row does) is out
of this task's scope; flagged here so it isn't lost.

## What's gated against a live override

Three separate places can push a value into the running process after
startup, and all three now respect the lock:

1. **`ritz_apply_config_live()`** (`main.cpp`) — the live-apply hook fired on
   every profile switch (`gamescope::config::SetLiveApplyHook()`). Every
   push with a matching `Opt` — filter/scaler/sharpness, adaptive
   sync/tearing/HDR, force-grab-cursor, force-grab-keyboard,
   force-windows-fullscreen — is now wrapped in `if ( !Given( Opt::X ) )`.
   Harmless at the true startup call (`apply_ritz_config_to_startup_state()`
   calls this function with `bStartup=true` **before** `main()`'s getopt
   loop has run, so every `Given()` reads false at that point and getopt's
   own assignment, which runs after, wins exactly as it always did).
2. **`apply_ritz_config_to_startup_state()`** (`main.cpp`) — the
   nested-width/height/refresh, force-windows-fullscreen-startup and
   force-grab-cursor-startup seeds also carry the same `Given()` guards, for
   symmetry and self-documentation, even though they are always no-ops at
   that specific call site (same reasoning as above: it runs before getopt).
3. **`overlay_e2_set` (`SetById()`, `Shell.cpp`)** — see below.

**Out of scope, noted rather than fixed:** a direct ConVar write from the
debug console (`adaptive_sync 0`, `tearing_enabled 1`, `hdr_enabled 0`, ...)
bypasses the registry entirely and is **not** gated. The lock is a settings
overlay affordance, not a ConVar access-control layer; closing that gap
would mean threading the lock check into `ConVar<T>::operator=()` itself,
well beyond this task's scope.

## `overlay_e2_set`: refuses a launch-locked row, not every disabled row

`SetById()` (the `overlay_e2_set` ConCommand) now refuses to write a row
whose `Entry::IsLaunchLocked()` is true, logging a clear line naming the
lock reason. This is **narrower** than "refuse anything `DisabledReason()`
is non-empty for" — deliberately: `scripts/settings_audit.py` and
`scripts/effects-regression.sh` both drive rows through this exact command
with a "flip the gate row first, then set the target row" sequence (e.g.
`settings_audit.py` writes `gate_id` immediately before `row_id` for a gated
row), which depends on a merely-`DisabledUnless()`-gated row staying
writable through this path even while its *own* gate currently reads false.
A blanket refusal on any non-empty `DisabledReason()` would have broken that
pattern across the existing regression scripts without a full audit of
every gated row's call sites. A launch-option lock is different in kind —
nothing in-process can turn it back off during the session (see "no reset"
above), so there is no equivalent "flip the gate first" available at all,
and refusing it here is what makes the lock mean the same thing against a
stray `overlay_e2_set` that it already means against a profile switch.

## The tag

`DrawLaunchLockTag()` (`Shell.cpp`), modelled on the existing
inherited/overridden dot (`DrawInheritMark()`): drawn right after a row's
measured label, clamped to the label lane's own right edge. Unlike the dot
it is a short filled **badge** with text ("LAUNCH OPTION") rather than a
plain mark, because the point is to be *read* on sight, not merely noticed.
`Role::Warn` fill, `Role::WarnText` text — amber, this kit's "needs
attention, not broken" hue, deliberately not `Role::Danger` (the row isn't
in an error state, it's just not editable here).

Every launch-locked row is, by construction, also a *disabled* row, so by
the time the tag draws it is already inside that row's
`ui::ScopedDim(bDisabled)` at 0.55 alpha. A tag whose whole job is
explaining *why* the row is dim would defeat itself by fading into the same
grey — so it draws at **full opacity**, via a new `ui::ScopedUndim`
(`Colors.h`/`.cpp`): forces the shared dim factor back to 1.0 for the tag's
own draw and restores whatever it was afterward. This is the one deliberate
exception to "disabled means 0.55" anywhere in the kit.

If a row happens to be both profile-overridden and launch-locked (a game
profile stores its own value for a key a launch flag also pins), the tag's
x-offset adds the inherit dot's own footprint first so the two marks don't
draw on top of each other.

The Inspector's Configure page shows the same reason text (`DisabledReason()`
already flows there, in `Role::WarnText`) — no separate wiring needed, since
the lock reuses the registry's one existing "why is this row disabled"
mechanism (see next section).

## Registry: a second, independent disabled slot

`Entry::LockedByLaunchOption( pred, reason )` (`Registry.h`/`.cpp`) is
**not** folded into the existing `Entry::DisabledUnless()`. Several of the
rows this locks already carry their own, unrelated gate —
`display.filter.sharpness`'s `SharpnessApplies` ("only FSR/NIS sharpen"),
the Custom-resolution steppers' `ResolutionIsCustom` ("pick Custom above"),
`display.refresh.custom`'s `RefreshIsCustom` — and `DisabledUnless()`
*overwrites* whatever predicate/reason a row already holds (`m_Enabled`/
`m_sReason` are a single slot). Calling it twice would silently drop
whichever call registered first, which is exactly the bug a caller
combining the two conditions by hand would hit.

So `LockedByLaunchOption()` keeps its **own** `m_LaunchLockPred`/
`m_sLaunchLockReason` pair, and `Entry::DisabledReason()` simply ORs the
two: the row is disabled if either says so, and the launch wording wins
when both apply, since it names an actual fix ("remove it from the launch
options") where a row's own gate reason usually just explains why the
*control* is inert right now. `Entry::IsLaunchLocked()` is the narrower
query the tag and `SetById()` actually ask — true only while the row is
disabled *specifically* because of a flag, so a row disabled for an
unrelated reason still greys out but draws no tag and stays writable
through `overlay_e2_set`.

The predicate is caller-supplied (`std::function<bool()>`) rather than
`Registry.h` knowing about `LaunchOptions.h`/`Opt` directly — keeps the
registry (deliberately free of everything but plain, ImGui-free data)
decoupled from one concrete flag enum. A call site passes
`[]{ return LaunchOptions::Given( LaunchOptions::Opt::Filter ); }` and a
reason built from `LaunchOptions::Spelling()` of that flag.

Only `Entry` carries this hook, not `Parameter` — every row this task locks
is a top-level `Entry` (a `Choice`/`Slider`/`Stepper`/`Switch`), never a
`Param` nested under one, so adding the same slot to `Parameter` would be
speculative. If a future lockable setting turns out to be a `Param`, add it
there the same way.

## Tests

`tests/test_overlay_ui.cpp`:

- *"law: LockedByLaunchOption has no spelling without a reason"* — the same
  `Law::ReasonRequired` enforcement `DisabledUnless()` already has.
- *"registry: LockedByLaunchOption is a second slot -- it never clobbers an
  existing DisabledUnless"* — registers both on one row, in the order a real
  call site does, and checks all four truth-table cells (neither / gate only
  / lock only / both), including that the lock reason contains the flag's
  own spelling and that `IsLaunchLocked()` tracks the lock specifically, not
  the OR of the two.
- *"LaunchOptions: MarkGiven flips Given() and records the spelling, per
  option"* — pure bookkeeping, no getopt, no compositor.

`tests/meson.build` gained `LaunchOptions.cpp` in the plain-source list
`test_overlay_ui.cpp`'s target already pulls in (alongside `Keybinds.cpp`,
`SteamFriends.cpp`, ...).

## Known gaps

- The debug-console ConVar path (`adaptive_sync`, `tearing_enabled`,
  `hdr_enabled`) is not gated — see "What's gated" above.
- `--framerate-limit`'s own live effect is separately broken, pre-existing
  this task — see the `display.fps_limit` section above.
- `Spelling()` for a flag reached through a shared short/long `case` (`-w`,
  `-h`, `-r`, `-S`, `-F`, `-g`) is a canonical `"-x/--long-name"` string, not
  necessarily what the player actually typed — `getopt_long` doesn't expose
  which form matched for those.
