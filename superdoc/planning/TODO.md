# TODO

Open work that has no feature doc of its own yet. Delete an entry when it is done.

## Direct scanout: picture artifacts on entering and leaving fullscreen

The GPU crash is fixed (no WSI memory chain on output images under a host, see
[backend-wayland.md](../features/backend-wayland.md) "Direct scanout under a host"). What
remains: with the host's direct scanout on, the picture shows artifacts when entering and
leaving fullscreen, sometimes older frames. They persist with the crash fix and look the same
with matching `XR30` formats, so the format switch is not the cause. Reportedly absent under
`RADV_DEBUG=allbos` on gamescope, which suggests a similar buffer-lifetime or listing problem on
the read side; unconfirmed. The Diagnostics warning about scanout being off is back and can be silenced
([diagnostics.md](../features/diagnostics.md)); reconsider it if the artifacts prove to be
unfixable on the gamescope side.

## Audio capture: crash on shutdown

`CaptureProcess()` (`src/Audio/Capture.cpp:469`) calls `pw_stream_trigger_process( p->pPlay )`
on PipeWire's data-loop thread. Teardown destroys `pPlay` before `pCap` (`:833`, `:839`), so the
capture callback can still fire with `pPlay` gone. Seen 2026-11-06 as a SIGSEGV in
`pw_stream_trigger_process` (segfault at 0x760) just after `steamcompmgr thread joined; killing
children`. Exit only, no GPU involvement. Likely fix: destroy `pCap` first, or guard the null.
