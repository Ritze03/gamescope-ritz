# TODO

Open work that has no feature doc of its own yet. Delete an entry when it is done.

## Direct scanout crash: run the `RADV_DEBUG=allbos` test

Background: [backend-wayland.md](../features/backend-wayland.md)'s known issue under "Direct
scanout under a host".

Launch with `RADV_DEBUG=allbos`, `render:direct_scanout = 1`, and spam fullscreen for at least
300 entries, since the crash averaged about one per 70. The flag forces every buffer object into
every submission, so it needs no code change. It also reaches the game, so frame rates mean
nothing during this run.

- **No crash:** a buffer object was missing from a submission. Find which one and why, then fix
  it in gamescope or report it to Mesa.
- **Crash:** the buffer list is not the cause. It is most likely a kernel or driver problem with
  shared buffers under scanout: report it to amdgpu, and look for a way to keep gamescope's
  output images off the plane.

`Why this test:` the fault is a write to a page that is invalid but still writable
(`PERMISSION_FAULTS=0x1`). A freed mapping is written as PTE 0, which would report `0x5`, so this
is a live mapping. `amdgpu_vm_handle_moved()` leaves exactly that PTE when it clears a moved
buffer object whose reservation someone else holds, and it repairs the ones listed in the
submission. That chain is a theory: nothing yet shows how an output image could be missing from
the list, and it does not explain why the game's gfx job hangs 2 s later.

Also open: the picture corrupts when entering and leaving fullscreen with scanout on. It looked
the same with matching `XR30` formats, so the format switch does not cause that either.

Ruled out so far: output image rotation and hold tracking, a freed image, and the format-change
modeset and swapchain rebuild. The `XR30` experiment is commit `93d4adb`, on the unmerged branch
`worktree-agent-afe98d0bed4914809`. The `allbos` test can use its binary or a normal build.

## Audio capture: crash on shutdown

`CaptureProcess()` (`src/Audio/Capture.cpp:469`) calls `pw_stream_trigger_process( p->pPlay )`
on PipeWire's data-loop thread. Teardown destroys `pPlay` before `pCap` (`:833`, `:839`), so the
capture callback can still fire with `pPlay` gone. Seen 2026-11-06 as a SIGSEGV in
`pw_stream_trigger_process` (segfault at 0x760) just after `steamcompmgr thread joined; killing
children`. Exit only, no GPU involvement. Likely fix: destroy `pCap` first, or guard the null.
