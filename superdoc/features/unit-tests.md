# Unit tests (`tests/gamescope_tests`)

The Catch2 unit-test binary `tests/gamescope_tests` and how it isolates its config directory.
Split out of [build-and-tooling.md](build-and-tooling.md) on 2026-11-07; the text below is moved verbatim.

`tests/meson.build`'s `gamescope_tests` is a single Catch2 binary covering every unit test
in `tests/`, run per-tag via `meson test` or directly as `./build-release/tests/gamescope_tests
[tag]` (no args runs everything). 2026-09-15: **the binary isolates `XDG_CONFIG_HOME` itself**
— `tests/test_global_isolation.cpp` registers a Catch2 event listener
(`CATCH_REGISTER_LISTENER`) that points `XDG_CONFIG_HOME` at a fresh run-wide temp directory
before the first test case and removes it after the last, re-asserting it before every test
case in between so a fixture's teardown can never leave a later test case pointed at the real
config home. This exists because `tests/test_keybinds.cpp`'s held-action and mouse-chord test
cases called `SetChord()`/`ResetAll()` (which persist via `Keybinds.cpp`'s `PersistLocked()`)
with no config-home override anywhere in that file, so a full run of the suite quietly
rewrote the developer's real `~/.config/gamescope-ritz/global.json` twice — found with
`inotifywait -m ~/.config/gamescope-ritz` around a run of the binary. The per-file
`TempConfigHome` fixtures (`test_config.cpp`, `test_resolution.cpp`,
`test_overlay_profiles.cpp`, `test_effects_curve.cpp`) still work unmodified on top of this
listener, but their own destructors unconditionally `unsetenv()` rather than restoring the
prior value, so they do not nest as cleanly on their own as their comments assume — the
listener's per-test-case re-assertion is what actually closes that gap for any test file,
present or future, that forgets its own isolation.

**Ad-hoc scripts must isolate `XDG_CONFIG_HOME` themselves** — the binary's own isolation
covers only `gamescope_tests`. `scripts/effects-regression.sh` and `scripts/pixel-regression.sh`
already do this (each launches gamescope with its own `CONFIGHOME` exported as
`XDG_CONFIG_HOME`); any new script or tool that runs gamescope or links `Config/ConfigManager.cpp`
needs the same. `tests/steam_friends_live_probe` (built but never registered as a `meson test`,
per its own header) does not link `ConfigManager.cpp` and touches no config path, so it needs
no such isolation.

## Related

- [build-and-tooling.md](build-and-tooling.md) — Meson build, feature flags, what is compiled in.
- [gentle-build.md](gentle-build.md) — builds and runs the tests the agent way.
