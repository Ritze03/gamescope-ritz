# Clipboard sync

Keeps one CLIPBOARD value in step across everything gamescope can reach: the
games running under Xwayland, gamescope's own native Wayland clients, the
overlay, and — when gamescope runs nested — the host session outside it.

Requested 2026-09-04. `PRIMARY` (the select-to-copy / middle-click buffer) is
deliberately **not** synced.
`Why:` primary updates continuously while you drag-select, which is a lot of
surface area for a buffer most users never think about. See
`superdoc/planning/requests-2026-09-04.md`.

## The four places a clipboard lives

| Holder | Owned by | Wired where |
| --- | --- | --- |
| Each Xwayland server's X11 `CLIPBOARD` selection | `steamcompmgr.cpp` | pre-existing, upstream |
| gamescope's own `wlr_seat` selection (native Wayland clients) | `wlserver.cpp` | **new** |
| The host compositor's clipboard (nested Wayland) | `Backends/WaylandBackend.cpp` | inbound **new**, outbound reworked |
| The host clipboard via SDL (nested SDL) | `Backends/SDLBackend.cpp` | pre-existing, now loop-guarded |

`gamescope_broadcast_clipboard()` in `steamcompmgr.cpp` is the single
funnel: whatever the origin, the text goes to all four. Every destination
drops a value it already holds, so the broadcast cannot loop (see
[The loop guard](#the-loop-guard)).

Because a clipboard transfer can only be produced on a worker thread (see
[Threading](#threading)), the entry point from anywhere else is
`gamescope_post_selection()`, a mailbox the steamcompmgr loop drains once per
iteration and then broadcasts.

## Which protocol is used where

Inbound — *learning that the host clipboard changed* — is the part that did not
exist. The Wayland backend picks the best available, best first:

1. **`ext_data_control_manager_v1`** — the standardised protocol, and the one
   that actually gets used on Hyprland, Sway, KWin ≥ 6.2, COSMIC and Niri.
2. **`zwlr_data_control_manager_v1`** — its wlroots predecessor, bound at
   version 1 (v2 only adds primary selection, which we do not want), for
   compositors that have not caught up.
3. **`wl_data_device`** — the ordinary clipboard, for hosts with neither
   (GNOME/Mutter).

1 and 2 are a **true event hook**: `selection` fires the instant the host
clipboard changes, focused or not. That is the behaviour the request asked for
first.

3 is the requested focus-based fallback, but delivered *by the protocol* rather
than by polling: `wl_data_device`'s `selection` event only arrives while
gamescope holds keyboard focus, so the host clipboard reaches us on focus gain
and no sooner. Nothing polls, and there is no focus-loss push either — the
outbound direction is not focus-gated in the first place.

**No data-control and no `wl_data_device`?** Clipboard sync with the host is
simply off, logged once at startup. The other three holders still sync among
themselves.

**Embedded (DRM) mode** has no host to sync with — `GetNestedHints()` returns
`nullptr` — so the feature is structurally inert there without needing a
check. Xwayland ↔ native-client ↔ overlay sync still works.

`ext_data_control_v1` and `zwlr_data_control_unstable_v1` are request-for-request
identical for everything used here, so `Clipboard/WaylandDataControl.h`
implements the logic once against a traits struct and stamps it out for both.

### Outbound is data-control too, when available

When a data-control manager is bound, *publishing* gamescope's clipboard to the
host also goes through it rather than through `wl_data_device`. That matters:
`wl_data_device_set_selection` needs a keyboard-enter serial, so the old path
could only publish while gamescope was focused. Data-control has no such
requirement, so a copy made inside gamescope is on the host clipboard
immediately.

### Protocol version floor

`ext-data-control` only landed in `wayland-protocols` 1.41, and
`wlr-data-control` has never been in `wayland-protocols` at all. Rather than
raise this project's floor to 1.41 for everyone, both XML files are **vendored
into `protocol/`** and listed in `protocol/meson.build` — the same treatment
`xdg-toplevel-icon-v1.xml` already gets. The dependency declaration is
unchanged.

## The loop guard

Clipboard sync is a cycle by construction: the host says its clipboard changed
→ we set the X11 selection → X notifies us the selection changed → we tell the
host → repeat.

The **X11 half** is already broken upstream by an identity check —
`handle_xfixes_selection_notify()` returns early when
`event->owner == ctx->ourWindow`.

The **Wayland half** has no identity check available. Data-control hands us an
offer with no way to ask "is this the source I created?", so the only signal is
the content itself. That turns out to be both sufficient and correct: *a
clipboard set to text it already holds is a no-op no matter who set it*, so
suppressing that transfer can never lose data.

`gamescope::CClipboardLoopGuard` (`Clipboard/ClipboardSync.h`) remembers the
last value that crossed in each direction and refuses to send a value straight
back the way it came:

- `ShouldPushToHost(t)` → false if `t` is what the host just gave us, or what we
  already pushed.
- `ShouldAcceptFromHost(t)` → false if `t` is our own push coming back, or a
  repeat of what we already took.

Each call clears the *opposite* direction's memory, so alternating between two
values (`a`, `b`, `a`, …) is correctly treated as three real changes rather than
mistaken for a loop. `tests/test_clipboard_sync.cpp` pins that case, along with
the two echo cases and `Reset()`.

The same guard instance is used by the **SDL backend**, which needs it for a
different reason: SDL raises `SDL_CLIPBOARDUPDATE` for gamescope's own
`SDL_SetClipboardText()` too, so without it a single copy ping-pongs forever.

`wlserver` has its own smaller version of the same idea:
`wlserver_set_selection()` skips taking the seat selection when the seat is
already offering that exact text, so the compositor never steals a client's
selection just to say the same thing back.

`wlserver_handle_request_set_selection()`'s listener on
`seat->events.request_set_selection` must be removed with `wl_list_remove()`
before `wl_display_destroy()` runs, in the same teardown block that already
removes `wlserver`'s other seat/backend listeners (`new_surface_listener`,
`new_input_listener`, etc. — see the bottom of the wlserver run loop in
`wlserver.cpp`). `wlr_seat_destroy()` (invoked from a `wl_display` destroy
listener wlroots installs internally) asserts every one of the seat's signal
lists is empty, so a listener left registered past that point aborts the
process on exit rather than merely leaking. This was missed when the listener
was first wired up and reached users as a shutdown crash on every clean exit;
fixed by adding it to the existing removal block.

Text is normalised before any of this: `NormalizeClipboardText()` strips
trailing NULs (X11 selection owners routinely NUL-terminate, Wayland ones never
do) so a round trip compares equal — without which the guard would not work at
all. Embedded NULs are left alone; truncating there would silently corrupt a
paste.

## Threading

**Decision: a short-lived detached worker thread per transfer, posting the
finished string back through a mutex-guarded mailbox.**

A clipboard transfer is a pipe: the receiver creates one, hands the write end to
the other party, and reads to EOF. The party on the far end is an arbitrary
application. `CWaylandBackend::PollState()` runs on the **steamcompmgr thread**,
which is the frame-pacing thread — a naive `read()` there stalls all of
gamescope for as long as some other app takes to write, and a `write()` on a
full 64 KiB pipe buffer stalls it for as long as some other app takes to read.

`Why a thread and not `O_NONBLOCK` plus the event loop:` the backend's
`PollState()` polls exactly one fd (the Wayland display) with a zero timeout and
has no general-purpose place to register another. A non-blocking transfer would
therefore have to be resumed across frames by a hand-rolled state machine, for
a transfer that finishes in microseconds in the normal case. A detached thread
that blocks on the pipe and drops the result into a mailbox is smaller and
*provably* cannot stall the frame loop, rather than merely being careful not to.

The thread is bounded on both axes so it cannot be turned into a leak by a
misbehaving peer:

- `ReadClipboardPipe()` / `WriteClipboardPipe()` `poll()` with
  `k_nClipboardTransferTimeoutMs` (2 s) and abandon a peer that goes quiet
  without closing.
- `k_nMaxClipboardBytes` (16 MiB) caps a single transfer, so a source that
  never stops writing cannot drive an unbounded allocation.

The mailbox in the other direction is the reason `gamescope_post_selection()`
exists: `gamescope_set_selection()` talks to every Xwayland server over its X
connection, and `wlserver_set_selection()` touches the seat, neither of which
may be done from a worker.

## Bugs fixed along the way

Two pre-existing defects were in the path of this work:

- **`handle_selection_notify()`** (`steamcompmgr.cpp`) called *either*
  `hints->SetSelection()` *or* `gamescope_set_selection()`. Nested, it took the
  first branch only — so a copy made in one Xwayland server was never pastable
  in another. It now always broadcasts locally *and* pushes to the host.
- **`Wayland_DataSource_Send()`** (`WaylandBackend.cpp`) dereferenced
  `m_pClipboard` unguarded (a null deref if a `send` arrived before anything had
  been copied) and did a blocking `write()` on the compositor thread. Both
  fixed; the write now goes to a worker.

## The overlay's Copy button

`Overlay/PanelLog.cpp`'s "Copy to clipboard" (issue #81) shipped **disabled**,
because the old implementation called `ImGui::SetClipboardText()` and — with no
`io.SetClipboardTextFn` wired for the overlay's context — wrote to an internal
buffer nothing outside the process could read.

The fix was never an ImGui clipboard handler. gamescope *is* the compositor, so
the button now calls `gamescope_post_selection()` and the log text goes onto the
real clipboard, reachable from a game, from another gamescope window, and from
the host session when nested.

## Settings

The System tab (`system.general`, `Overlay/PanelSystem.cpp` — the first area in
the SYSTEM rail section) has a "Clipboard sync" switch and a read-only status
row underneath it. Phase A of that tab; see
`superdoc/planning/requests-2026-09-05.md` item 5.

**The switch** (`system.clipboard_sync`, default on). Off stops the *inbound*
half of sync: a host clipboard change never reaches a client inside gamescope.
Concretely, three call sites check `gamescope::g_bClipboardSyncEnabled`
(`Clipboard/ClipboardSync.h`):

- `CWaylandBackend::DrainHostClipboard()` — the mailbox a transfer worker fills
  is still drained every `PollState()` even while off, but the value is
  dropped rather than broadcast. `Why:` draining and discarding, rather than
  leaving the mailbox alone, is what keeps a stale host value from firing the
  instant sync is turned back on.
- `CDataControlDevice::OnSelection()` (`Clipboard/WaylandDataControl.h`) and
  `CWaylandBackend::Wayland_DataDevice_Selection()` — both skip calling
  `receive()`/`wl_data_offer_receive()` at all while off, not just the
  broadcast that would normally follow. So host clipboard text is never read
  into the process in the first place, which is the stronger, privacy-shaped
  guarantee: a game or another gamescope client cannot observe host clipboard
  contents through some other channel while the switch is off, because
  gamescope itself never received them.
- `CSDLBackend`'s `SDL_CLIPBOARDUPDATE` handler — skips
  `gamescope_post_selection()` when off (SDL hands over the text as a side
  effect of the event itself, so there is no separate receive step to skip
  the way there is on Wayland).

Copying and pasting *inside* gamescope — between an Xwayland game, a native
Wayland client, and the overlay — is unaffected either way; only the host
boundary is gated.

The *outbound* half (a copy made inside gamescope reaching the host) is not
gated by this switch yet — that is Phase B, at `steamcompmgr.cpp`'s
`gamescope_broadcast_clipboard()` (`hints->SetSelection()`).

`Why: ignore the data-control device rather than destroy it` — turning the
switch off does not tear down `m_ExtDataControl`/`m_WlrDataControl` or the
`wl_data_device`. Destroying either needs a full re-`Init()` to come back
(`WaylandDataControl.h`'s `Shutdown()`, driven by the compositor's own
`finished` event via `OnFinished()`), and the `wl_data_device` fallback path
has no re-creation story at all once torn down (`WaylandBackend.cpp`'s
`InitClipboard()` decides the protocol once, before steamcompmgr exists). A
plain runtime flag that every read path checks is one atomic, always safe to
flip from any thread, and trivially reversible — destroying and rebuilding a
live Wayland protocol object for what is, in effect, a checkbox would trade
that simplicity for a re-init path this feature does not otherwise need.

**The re-enable caveat.** Turning the switch back on does not retroactively
fetch the host's current clipboard value — data-control and `wl_data_device`
are both push protocols with no "give me your current selection" request, so
there is nothing to poll. The host value gamescope holds is whatever it was
when sync was last on, and stays that way until the host clipboard next
changes (or, on the `wl_data_device` fallback, until gamescope next gains
focus).

**The status row** (`system.clipboard_status`, read-only) names the protocol
actually in use, from `INestedHints::GetClipboardSyncStatus()`
(`backend.h`) — `ext_data_control_v1`, `zwlr_data_control_v1`,
`wl_data_device (focus-based)`, `SDL clipboard`, or `none`. Decided once by
each backend's `InitClipboard()`/connector construction, before steamcompmgr
exists, so every later read (from any thread) sees a value nothing writes
again — it does not track a data-control device being revoked after the
fact. In embedded (DRM) mode, where `GetNestedHints()` returns `nullptr`
(same discriminator `gamescope_broadcast_clipboard()` uses), the panel reports
`inert: no host (embedded)` instead of calling into a hint object that does
not exist. The area's own summary line in the rail folds both rows into one
phrase: `clipboard sync on · <protocol>`, or `clipboard sync off`.

**Not persisted yet.** Phase A's switch only flips the runtime atomic — the
value does not survive a restart. `config::SystemSettings::clipboard_sync` is
the Phase B seam that fixes that; see `Overlay/PanelSystem.cpp`'s setter
comment.

## Where the code is

| File | What |
| --- | --- |
| `src/Clipboard/ClipboardSync.{h,cpp}` | Loop guard, text normalisation, bounded pipe read/write, and `g_bClipboardSyncEnabled`. No compositor dependency; unit-tested. |
| `src/Overlay/PanelSystem.{h,cpp}` | The System tab's "Clipboard sync" switch and status row. |
| `src/Clipboard/WaylandDataControl.h` | The `ext_`/`zwlr_` data-control device, written once against a traits struct. |
| `src/backend.h` | `INestedHints::GetClipboardSyncStatus()` -- the status row's data source. |
| `src/Backends/WaylandBackend.cpp` | Protocol selection, the `wl_data_device` fallback, inbound mailbox. |
| `src/Backends/SDLBackend.cpp` | SDL clipboard, now loop-guarded. |
| `src/wlserver.cpp` | Seat `request_set_selection` handling and the compositor-owned `wlr_data_source`. |
| `src/steamcompmgr.cpp` | X11 selection ownership (upstream) plus the broadcast funnel and cross-thread mailbox. |
| `protocol/{ext-data-control-v1,wlr-data-control-unstable-v1}.xml` | Vendored protocol definitions. |
| `tests/test_clipboard_sync.cpp` | Loop guard, normalisation, pipe transfer. |
