# Input Method / On-Screen Keyboard — IME bridge

A small bridge that lets an on-screen-keyboard-style client act as a Wayland input method
for whatever surface currently has focus inside Gamescope — typing text and issuing
actions (e.g. commit/close) on the client's behalf.

## How it works

- `create_ime_manager(wlserver_t *wlserver)` (`src/ime.hpp:8`) installs the input-method
  manager on the server, implementing the `gamescope_input_method` Wayland protocol
  defined in `protocol/gamescope-input-method.xml`.
- `create_local_ime()` / `destroy_ime(...)` create and tear down a local input-method
  context; `type_text(ime, text)` pushes UTF-8 text through it.
- `perform_action(wlserver_input_method *ime, enum gamescope_input_method_action action)`
  (`src/ime.hpp:13`) issues a protocol-defined action (the `gamescope_input_method_action`
  enum comes from the generated protocol header, `gamescope-input-method-protocol.h`).
  *Why:* text input and discrete actions are separate calls because the underlying
  protocol models them as distinct requests — typing is a stream, actions are one-shot
  intents (e.g. dismiss).

## Using it

Any client implementing the `gamescope_input_method` protocol (an on-screen keyboard app)
binds to it, receives focus/activation for the current text input, and calls `type_text`/
`perform_action`-equivalent protocol requests to feed text into whatever has keyboard
focus in Gamescope.

## Related links

- [wayland-protocols](wayland-protocols.md) — protocol XML conventions used across Gamescope's custom Wayland extensions.
- [steamcompmgr-focus](steamcompmgr-focus.md) — how keyboard focus is tracked, which determines which surface IME input is delivered to.
