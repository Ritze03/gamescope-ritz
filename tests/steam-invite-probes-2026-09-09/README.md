# Steam invite / vtable-layout probes — 2026-09-09

**Throwaway research probes, deliberately not wired into `meson.build`.** They
are here so a clean rebuild does not delete them along with
`build-release/verify-shots/steam-invite-2026-09-09/`, where they were written
and where their output lives.

What they establish, and the raw evidence, is
[`superdoc/planning/steam-invite-and-vtable-layout.md`](../../superdoc/planning/steam-invite-and-vtable-layout.md).
Read that first; this is only the toolbox.

| file | what it does | touches the live client? |
|---|---|---|
| `extract-vtable-slots.py` | Reads an interface's whole vtable slot map out of Proton's `lsteamclient.so` by disassembly. | no — a file |
| `extract-flat-slots.py` | The same, out of a game's shipped `libsteam_api.so` flat API. | no — a file |
| `cross-build.sh` | Runs the first across every Proton build on this machine and tabulates the agreement. | no |
| `version-lengths.sh` | Live vtable length per interface version vs Proton's method count. | reads only |
| `vtable_read_probe.cpp` | Reads the live client's vtable pointer arrays out of memory and cross-aligns two interface versions. **Calls no vtable slot.** Every address is checked against `/proc/self/maps` first. | reads only |
| `decode-thunks.py` | Decodes each vtable thunk's inner-implementation slot and aligns `SteamFriends018` against `SteamFriends017` by it. | no — a file |
| `invite_probe.cpp` | Re-derives the layout in-process (78 slots; inner slots 7/10/23/215) and refuses to go on unless it matches. Default run is read-only. `--invite <persona>` performs **one** write and only after the persona resolves to exactly one friend. | reads; `--invite` writes |

Build either probe standalone:

```
g++ -O1 -g -o vtable_read_probe vtable_read_probe.cpp -ldl
g++ -O1 -g -o invite_probe      invite_probe.cpp      -ldl
```

`invite_probe --invite …` was **never run** — the session's permission system
refused it, which is the right default for a call that reaches another person's
Steam client. See the planning page's §8.
