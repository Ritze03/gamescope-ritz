# Screen Capture (PipeWire) — streaming export of the composited output

Exposes Gamescope's composited output as a PipeWire video stream, so external consumers
(screen recorders, remote-play/streaming tools, screenshot tools) can capture what's on
screen without reading back from the display hardware themselves.

## How it works

- `init_pipewire(void)` (`src/pipewire.hpp:58`, implemented `src/pipewire.cpp:671`) sets
  up the `pipewire_state`, creates a `pw_loop`, and spawns a **dedicated OS thread**
  running `run_pipewire` (`src/pipewire.cpp:746 std::thread thread(run_pipewire, state)`).
  That thread is named `"gamescope-pw"` (`pthread_setname_np`, `src/pipewire.cpp:622`) and
  runs its own blocking `poll()` loop (`src/pipewire.cpp:632 poll(pollfds, EVENT_COUNT,
  -1)`) over two fds — the PipeWire loop's fd and an internal "nudge" pipe used to wake it
  — calling `pw_loop_iterate` when the PipeWire fd is readable. This is a real dedicated
  thread, not an `IWaitable` plugged into Gamescope's shared epoll waiter machinery.
  *Why:* PipeWire's own loop object (`pw_loop`) expects to own its iterate cycle, so
  giving it a private thread with a private `poll()` avoids coupling PipeWire's dispatch
  cadence to Gamescope's compositor-thread waiter.
- `pipewire_is_streaming()` (`src/pipewire.hpp:61`, implemented `src/pipewire.cpp:757`)
  lets the rest of Gamescope cheaply check whether a client is actively consuming the
  stream (e.g. to skip capture-buffer work when nobody is watching).
- Buffers cross threads via a queue: `dequeue_pipewire_buffer()` /
  `push_pipewire_buffer()` / `pipewire_destroy_buffer()` hand frame buffers between the
  compositor thread (which fills them from the composited output) and the PipeWire thread
  (which submits them to the stream); `nudge_pipewire(void)` writes to the nudge pipe to
  wake the PipeWire thread's `poll()` promptly instead of waiting for its own timeout.
- `get_pipewire_stream_node_id(void)` returns the PipeWire node ID that Wayland clients
  need to attach to the right stream; that ID is advertised to clients over the
  `gamescope_pipewire` protocol's `stream_node` event
  (`protocol/gamescope-pipewire.xml:33 <event name="stream_node">`).
- The whole feature is compiled conditionally behind the Meson feature flag `pipewire`
  (`meson_options.txt:1`, "Screen capture via PipeWire").
- `src/Apps/gamescopestream.cpp` is a standalone example/reference client (ported from a
  PipeWire SPA example) showing how an external process consumes a Gamescope PipeWire
  stream and displays it via `libdecor`/Wayland dmabuf import.

## Threading model (verified)

Unlike the `IWaitable`-based subsystems in [input-emulation](input-emulation.md), PipeWire
streaming runs on its **own dedicated `std::thread`** (`src/pipewire.cpp:746`) with a
private `poll()` loop, not folded into Gamescope's shared `CAsyncWaiter`/epoll machinery.

## Using it

Build with `-Dpipewire=enabled`. At runtime, a Wayland client binds the `gamescope_pipewire`
global, does a roundtrip to receive the `stream_node` event, then connects to that PipeWire
node with the regular PipeWire client API to receive frames.

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `pipewire` (Meson feature) | auto | Builds PipeWire screen-capture support (`meson_options.txt:1`). |

## Related links

- [compositing-vulkan](compositing-vulkan.md) — produces the composited frame that gets copied into PipeWire buffers.
- [wayland-protocols](wayland-protocols.md) — the `gamescope_pipewire` protocol used to advertise the stream node.
- [input-emulation](input-emulation.md) — contrasting example of a subsystem that *is* integrated into the shared epoll waiter instead of running its own thread.
