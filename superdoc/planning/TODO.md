# TODO

Open work that has no feature doc of its own yet. Delete an entry when it is done.

## Direct scanout: picture artifacts on entering and leaving fullscreen

The GPU crash is fixed (no WSI memory chain on output images under a host, see
[backend-wayland.md](../features/backend-wayland.md) "Direct scanout under a host"). What
remains: with the host's direct scanout on, the picture shows artifacts when entering and
leaving fullscreen, sometimes older frames. They persist with the crash fix and look the same
with matching `XR30` formats, so the format switch is not the cause. Reportedly absent under
`RADV_DEBUG=allbos` on gamescope (the brief artifacts appear under both Hyprland and GNOME), which suggests a similar buffer-lifetime or listing problem on
the read side; unconfirmed. The Diagnostics warning about scanout being off is back and can be silenced
([diagnostics.md](../features/diagnostics.md)); reconsider it if the artifacts prove to be
unfixable on the gamescope side.

Separately, a user on GNOME (Mutter) saw the whole picture, menu included, scrambled into fine
block noise after tabbing out and back in. Cause unknown; candidates are host direct scanout, that
user's amdgpu VM setting, or a GPU or Mesa difference. The "Block direct scanout" switch
([display-general.md](../features/display-general.md#block-direct-scanout-displayblock_direct_scanout)) is the test for the first one:
if the noise goes away with it on, scanout is involved. Whether the hosts actually refuse scanout
with it is untested.

## Audio capture: crash on shutdown

`CaptureProcess()` (`src/Audio/Capture.cpp:469`) calls `pw_stream_trigger_process( p->pPlay )`
on PipeWire's data-loop thread. Teardown destroys `pPlay` before `pCap` (`:833`, `:839`), so the
capture callback can still fire with `pPlay` gone. Seen 2026-11-06 as a SIGSEGV in
`pw_stream_trigger_process` (segfault at 0x760) just after `steamcompmgr thread joined; killing
children`. Exit only, no GPU involvement. Likely fix: destroy `pCap` first, or guard the null.
